#include "client_runtime.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include "crate_catalogue.h"
namespace
{
    struct GemReward
    {
        const char *level;
        unsigned primary, extra;
    };
    constexpr std::array<GemReward, 85> gem_rewards{{
#include "gem_reward_specs.inc"
    }};
    void print_zone(const std::string &slot, const std::string &name, const save_probe::Zone &zone)
    {
        std::cout << "Save " << slot << " zone " << name << ": clears " << zone.completions << ", relic " << zone.relic
                  << ", collectibles 0x" << std::hex << zone.collectibles << std::dec << std::endl;
    }

    using save_probe::level_key;

    std::vector<CheckKey> candidate_checks(const std::string &zone, const save_probe::Zone &record)
    {
        const auto level = level_key(zone);
        if (level.empty())
            return {};
        std::vector<CheckKey> result;
        if (record.completions)
            result.emplace_back(level, "clear");
        const auto reward = std::find_if(gem_rewards.begin(), gem_rewards.end(),
                                         [&](const auto &entry) { return level == entry.level; });
        if (reward != gem_rewards.end())
        {
            if (record.collectibles & reward->primary)
                result.emplace_back(level, "gem");
            if (record.collectibles & reward->extra)
                result.emplace_back(level, "extra_gem");
        }
        if ((level[1] == '2' || level[1] == '3') && (record.collectibles & 0x200))
            result.emplace_back(level, "crystal");
        if (record.relic)
            result.emplace_back(level, "relic");
        return result;
    }

    std::set<std::string> save_clears(const save_probe::Zones &zones, char campaign,
                                      const std::set<std::string> &selected)
    {
        std::set<std::string> result;
        for (const auto &[name, zone] : zones)
        {
            const auto level = level_key(name);
            if (zone.completions && selected.count(level) && level[1] == campaign)
                result.insert(level);
        }
        return result;
    }

    bool save_slot_filename(const std::string &name)
    {
        if (name.size() < 10 || name.compare(0, 5, "SAVE_") != 0 || name.compare(name.size() - 4, 4, ".sav") != 0)
            return false;
        return std::all_of(name.begin() + 5, name.end() - 4, [](char digit) { return digit >= '0' && digit <= '9'; });
    }

} // namespace
void ClientRuntime::update_saves(ClientTick &tick)
{
    tick.seed_matches = tick.room_ready && (!bound_saves.empty() || (cloud && cloud->ready()));
    if (tick.room_ready && tick.online)
    {
        AP_RoomInfo room{};
        if (AP_GetRoomInfo(&room) == 0 && room.seed_name == session.seed())
        {
            tick.seed_matches = true;
            previous_seed_error.clear();
        }
        else if (previous_seed_error != "AP room seed differs from the active session")
        {
            previous_seed_error = "AP room seed differs from the active session";
            std::cerr << previous_seed_error << std::endl;
        }
    }
    if (tick.status != AP_ConnectionStatus::Authenticated)
    {
        submitted_checks.clear();
        goal_sent = false;
    }

    std::set<std::string> completed_levels;
    if (tick.room_ready)
    {
        const auto queued = session.pending();
        const auto local_clears = session.local_clears();
        for (const auto &[key, id] : tick.checks)
            // server checks can be released, only local events establish a clear.
            if (key.second == "clear" && (queued.count(id) || local_clears.count(id)))
                completed_levels.insert(key.first);
        if (tick.online)
            for (const auto id : queued)
                if (submitted_checks.insert(id).second)
                    AP_SendItem(id);
    }

    std::set<std::int64_t> save_checks;
    if (!save_dir.empty() && std::filesystem::exists(save_dir))
    {
        try
        {
            for (const auto &entry : std::filesystem::directory_iterator(save_dir))
            {
                if (!entry.is_regular_file())
                    continue;
                const auto slot = entry.path().filename().string();
                if (!save_slot_filename(slot))
                    continue;
                try
                {
                    auto current = save_probe::zones(save_probe::read_file(entry.path()));
                    const bool room_save = (seed ? seed->shuffle : 0) == 2 &&
                                           std::any_of(bound_saves.begin(), bound_saves.end(),
                                                       [&](const auto &bound) { return bound.second == slot; });
                    if (tick.seed_matches)
                    {
                        for (const auto &[campaign, bound_slot] : bound_saves)
                        {
                            if (bound_slot != slot)
                                continue;
                            const auto clears = save_clears(current, campaign, tick.levels);
                            completed_levels.insert(clears.begin(), clears.end());
                            if (room_save)
                                for (char other = '1'; other <= '3'; ++other)
                                {
                                    const auto extra = save_clears(current, other, tick.levels);
                                    completed_levels.insert(extra.begin(), extra.end());
                                }
                        }
                    }
                    const auto previous = previous_zones.find(slot);
                    for (const auto &[name, zone] : current)
                    {
                        if (previous == previous_zones.end())
                        {
                            if (zone.completions || zone.collectibles)
                                print_zone(slot, name, zone);
                        }
                        else
                        {
                            const auto old = previous->second.find(name);
                            if (old == previous->second.end() || old->second != zone)
                                print_zone(slot, name, zone);
                        }
                        if (tick.room_ready)
                        {
                            for (const auto &key : candidate_checks(name, zone))
                            {
                                const auto campaign_binding = bound_saves.find(key.first[1]);
                                if (tick.seed_matches &&
                                    (room_save || (campaign_binding != bound_saves.end() && campaign_binding->second == slot)))
                                {
                                    const auto spec = ingame::collectible_spec(key.first.c_str());
                                    if (spec)
                                        for (unsigned i = 0; i < 4; ++i)
                                            if (key.second == ingame::collectible_events[i])
                                            {
                                                const auto id = ingame::collectible_location(*spec, i);
                                                if (session.slot_data()["local_reward_sources"].isMember(
                                                        std::to_string(id)))
                                                    session.observe_collectible(id);
                                            }
                                }
                                const auto found = tick.checks.find(key);
                                if (found == tick.checks.end() || (key.second != "clear" && tick.checked.count(found->second)))
                                    continue;
                                // received collectibles also set save bits.
                                // pickup events alone only award the location
                                if (session.slot_data()["collectible_locations"].isMember(
                                        std::to_string(found->second)))
                                    continue;
                                const auto bound = bound_saves.find(key.first[1]);
                                if (tick.seed_matches &&
                                    (room_save || (bound != bound_saves.end() && bound->second == slot)))
                                {
                                    save_checks.insert(found->second);
                                }
                                else if (displayed_candidates[slot].insert(found->second).second)
                                {
                                    std::cout << "Candidate AP check " << found->second << " (" << key.first << ' '
                                              << key.second << ") from " << slot << " (read-only, not sent)"
                                              << std::endl;
                                }
                            }
                        }
                    }
                    previous_zones[slot] = std::move(current);
                    previous_save_errors.erase(slot);
                }
                catch (const std::exception &error)
                {
                    if (previous_save_errors[slot] != error.what())
                    {
                        std::cerr << "Save " << slot << " probe: " << error.what() << std::endl;
                        previous_save_errors[slot] = error.what();
                    }
                }
            }
        }
        catch (const std::exception &error)
        {
            if (previous_save_errors[""] != error.what())
            {
                std::cerr << "Save directory: " << error.what() << std::endl;
                previous_save_errors[""] = error.what();
            }
        }
    }
    try
    {
        if (!save_checks.empty())
            for (auto id : session.queue(save_checks))
            {
                std::cout << "AP save check earned: " << id << '\n';
                if (tick.online && submitted_checks.insert(id).second)
                    AP_SendItem(id);
            }
    }
    catch (const std::exception &error)
    {
        if (previous_save_errors["persistence"] != error.what())
            std::cerr << "Save check persistence: " << error.what() << std::endl;
        previous_save_errors["persistence"] = error.what();
    }
    const auto *goals = tick.room_ready && seed ? &seed->goals : nullptr;
    if (!tick.seed_matches || !goals)
    {
        goal_sent = false;
        previous_goal_progress.clear();
    }
    std::string goal_hud;
    if (goals && tick.seed_matches)
    {
        if (goals->type != "cortex" && goals->type != "all_bosses" && goals->type != "cortex_last" && goals->type != "cortex_requirements")
        {
            const auto count = goal_progress(*goals, completed_levels, tick.checks, tick.checked, tick.items);
            if (goals->type == "token_hunt")
                goal_hud = "AP Tokens: " + std::to_string(count) + '/' + std::to_string(goals->required);
            const auto progress = std::to_string(count) + '/' + std::to_string(goals->required) +
                                  (goals->type == "token_hunt" ? " AP Tokens received"
                                                               : " distinct regular levels cleared and acknowledged");
            if (previous_goal_progress['G'] != progress)
                print_console("AP goal: " + progress, false);
            previous_goal_progress['G'] = progress;
        }
        if (goals->type == "all_bosses" || goals->type == "cortex_last" || goals->type == "cortex_requirements")
            goal_hud = "Bosses: " +
                       std::to_string(goal_progress(*goals, completed_levels, tick.checks, tick.checked, tick.items)) +
                       '/' + std::to_string(goals->levels.size());
        if (goals->type == "cortex_requirements")
        {
            for (const auto &[kind, target] : goals->requirements)
                goal_hud += std::string(kind == "token_hunt" ? " | Tokens:" : " | Clears:") +
                            std::to_string(requirement_progress(seed->rules, kind, tick.items, tick.checked)) + '/' +
                            std::to_string(target);
            if (previous_goal_progress['G'] != goal_hud)
                print_console("AP goal: " + goal_hud + ". All unlock requirements are required before Cortex.", false);
            previous_goal_progress['G'] = goal_hud;
        }
        for (char campaign = '1'; campaign <= '3'; ++campaign)
        {
            unsigned total = 0, cleared = 0;
            for (const auto &level : goals->levels)
                if (level[1] == campaign)
                {
                    ++total;
                    const auto check = tick.checks.find({level, "clear"});
                    cleared += completed_levels.count(level) && check != tick.checks.end() &&
                               tick.checked.count(check->second);
                }
            if (!total)
                continue;
            const auto bound = bound_saves.find(campaign);
            const auto progress = std::to_string(cleared) + '/' + std::to_string(total) +
                                  " goal bosses cleared and acknowledged" +
                                  (bound == bound_saves.end() ? ". Campaign save is not bound" : "");
            if (previous_goal_progress[campaign] != progress)
                print_console(std::string("Crash ") + campaign + " goal: " + progress, false);
            previous_goal_progress[campaign] = progress;
        }
        if (tick.online && !goal_sent &&
            goals_complete(*goals, completed_levels, tick.checks, tick.checked, tick.items))
        {
            AP_StoryComplete();
            goal_sent = true;
            print_console("AP goal sent: " + goals->type + " victory condition completed.", false);
        }
    }
    if (box_markers)
        box_markers->goal(goal_hud);
}

void ClientRuntime::drain_crates(ClientTick &tick)
{
    if (crate_events)
    {
        try
        {
            for (auto event : crate_events->drain())
            {
                std::transform(event.zone.begin(), event.zone.end(), event.zone.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                auto zone = event.zone;
                if (zone.rfind("maps/", 0) == 0)
                    zone = zone.substr(5);
                const auto level = level_key(zone);
                if (!tick.seed_matches || !tick.levels.count(level))
                    continue;
                const auto spec = std::find_if(std::begin(box_specs), std::end(box_specs), [&](const auto &box) {
                    return level == box.level && event.name == box.name && event.position == box.position;
                });
                if (spec == std::end(box_specs))
                    continue;
                const auto check = tick.checks.find({level, spec->event_name()});
                if (check == tick.checks.end() || check->second != spec->id)
                    continue;
                crate_checks.insert(check->second);
            }
        }
        catch (const std::exception &error)
        {
            if (crate_error != error.what())
                std::cerr << "Box observer: " << error.what() << std::endl;
            crate_error = error.what();
            crate_events.reset();
            gem_cache = {};
        }
    }
    if (tick.seed_matches && !crate_checks.empty())
    {
        try
        {
            for (auto id : session.queue(crate_checks))
            {
                std::cout << "AP box check earned: " << id << std::endl;
                if (tick.online && submitted_checks.insert(id).second)
                    AP_SendItem(id);
            }
            crate_checks.clear();
        }
        catch (const std::exception &error)
        {
            // drained events must survive a failed disk write so the next pass can retry
            if (crate_error != error.what())
                std::cerr << "Box persistence: " << error.what() << std::endl;
            crate_error = error.what();
        }
    }
}
