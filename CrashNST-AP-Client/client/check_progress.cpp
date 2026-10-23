#include "check_progress.h"
#include "ingame_protocol.h"
#include "hub_access.h"
#include "game_hooks.h"
#include <algorithm>
#include <stdexcept>

namespace
{
    struct LogicLevel
    {
        const char *key;
        const char *title;
        std::map<std::string, std::vector<std::int64_t>> rules;
    };
    const LogicLevel catalogue[]{
#include "check_logic_specs.inc"
    };
    const LogicLevel *level_logic(const std::string &key)
    {
        const auto found = std::find_if(std::begin(catalogue), std::end(catalogue),
                                        [&](const auto &entry) { return key == entry.key; });
        return found == std::end(catalogue) ? nullptr : found;
    }
    std::set<std::int64_t> known_items()
    {
        std::set<std::int64_t> result;
        for (const auto &level : catalogue)
            for (const auto &[event, ids] : level.rules)
                result.insert(ids.begin(), ids.end());
        for (unsigned game = 1; game <= 3; ++game)
            for (unsigned area = 2; area <= (game == 1 ? 3u : 5u); ++area)
                result.insert(hub_access_item(game, area));
        return result;
    }
    std::map<std::int64_t, unsigned> item_counts(const Json::Value &rule, const std::set<std::int64_t> &valid)
    {
        if (!rule.isArray() || rule.size() > 40)
            throw std::runtime_error("Invalid alternate route requirements");
        std::map<std::int64_t, unsigned> result;
        for (const auto &item : rule)
            if (!item.isInt64() || !valid.count(item.asInt64()) || ++result[item.asInt64()] > hub_item_limit(item.asInt64()))
                throw std::runtime_error("Unknown alternate route requirement");
        return result;
    }
} // namespace

unsigned box_marker_color(bool spoilers, const Json::Value &location_flags, std::int64_t location)
{
    if (!spoilers || !location_flags.isObject())
        return 0;
    const auto &flags = location_flags[std::to_string(location)];
    if (!flags.isUInt() || flags.asUInt() > 7)
        return 0;
    return ingame::item_color((flags.asUInt() & 4) ? 1u : flags.asUInt());
}
std::string CheckCounts::text() const
{
    return std::to_string(done) + "/" + (logic_known ? std::to_string(in_logic) : "?") + " (" + std::to_string(total) +
           ")";
}
std::string LevelProgress::collectible_text() const
{
    std::string result;
    const char *labels[]{"C", "P", "R"};
    for (unsigned i = 0; i < collectibles.size(); ++i)
        if (collectibles[i].total)
        {
            if (!result.empty())
                result += "   ";
            result += std::string(labels[i]) + ": " + collectibles[i].text();
        }
    return result;
}
std::array<LevelProgress, 3> campaign_progress(const CheckProgress &progress,
                                               const std::map<std::string, std::string> &placements)
{
    std::array<LevelProgress, 3> result{};
    std::map<std::string, std::string> entrances;
    for (const auto &[source, destination] : placements)
        entrances[destination] = source;
    for (const auto &[destination, level] : progress.levels)
    {
        auto parent = destination;
        for (const auto &[secret, source] : secret_stages)
            if (destination == secret)
                parent = source;
        const auto found = entrances.find(parent);
        const auto &source = found == entrances.end() ? destination : found->second;
        if (source.size() != 4 || source[1] < '1' || source[1] > '3')
            continue;
        auto &campaign = result[source[1] - '1'];
        campaign.done += level.done;
        campaign.in_logic += level.in_logic;
        campaign.total += level.total;
        campaign.logic_known = campaign.logic_known && level.logic_known;
    }
    return result;
}

CheckRules compile_check_rules(const std::map<std::pair<std::string, std::string>, std::int64_t> &checks,
                               const Json::Value &slot_data, const std::set<std::string> &selected)
{
    CheckRules result;
    const auto &victory = slot_data["victory_goal"];
    if (victory["type"] == "cortex_requirements")
    {
        for (const auto &kind : victory["requirements"].getMemberNames())
            result.cortex_requirements[kind] = victory["requirements"][kind].asUInt();
        for (const auto &goal : slot_data["goal_levels"])
            if (goal == "B106" || goal == "B205" || goal == "B305")
                result.gated_finals.insert(goal.asString());
        for (const auto &[key, id] : checks)
            if (key.first[0] == 'L' && key.second == "clear")
                result.regular_clears.insert(id);
    }
    if (victory["type"] == "cortex_last" || victory["boss_mode"] == "cortex_last")
    {
        const auto &goals = slot_data["goal_levels"];
        if (!goals.isArray() || goals.size() > 16)
            throw std::runtime_error("Invalid Cortex-last goal bosses");
        for (const auto &goal : goals)
        {
            if (!goal.isString())
                throw std::runtime_error("Invalid Cortex-last goal boss");
            const auto final = goal.asString();
            if (final != "B106" && final != "B205" && final != "B305")
                continue;
            auto &required = result.boss_requirements[final];
            for (char number = '1'; number < final[3]; ++number)
            {
                const auto boss = std::string("B") + final[1] + '0' + number;
                const auto check = checks.find({boss, "clear"});
                if (check == checks.end())
                    throw std::runtime_error("Cortex-last prerequisite clear is missing");
                required.insert(check->second);
            }
        }
    }
    const auto &rules = slot_data["check_requirements"];
    static const auto valid_items = known_items();
    const bool rules_valid = rules.isObject() && rules.size() == checks.size();
    const auto &alternate_rules = slot_data["alternate_check_requirements"];
    bool alternatives_valid = alternate_rules.isNull() || (alternate_rules.isObject() && alternate_rules.size() <= checks.size());
    if (alternate_rules.isObject())
    {
        std::set<std::string> check_ids;
        for (const auto &entry : checks) check_ids.insert(std::to_string(entry.second));
        for (const auto &id : alternate_rules.getMemberNames())
            alternatives_valid = alternatives_valid && check_ids.count(id);
    }
    if (!rules_valid)
        result.error = "Invalid check_requirements. Reachable markers hidden";
    {
        const auto &local_sources = slot_data["local_reward_sources"];
        try
        {
            if (!local_sources.isObject() || local_sources.size() > 4 * ingame::max_levels)
                throw std::runtime_error("Invalid local collectible rules");
            for (const auto &id : local_sources.getMemberNames())
            {
                std::size_t used = 0;
                const auto address = std::stoll(id, &used);
                if (used != id.size() || std::to_string(address) != id)
                    throw std::runtime_error("Invalid local collectible identity");
                ingame::local_collectible_item(slot_data, address);
                if (!selected.count(local_sources[id]["level"].asString()))
                    throw std::runtime_error("Local collectible outside selected levels");
                const auto &rule = local_sources[id]["requirements"];
                if (!rule.isArray() || rule.size() > 40)
                    throw std::runtime_error("Invalid local collectible requirements");
                std::map<std::int64_t, unsigned> counts;
                for (const auto &item : rule)
                    if (!item.isInt64() || !valid_items.count(item.asInt64()) ||
                        ++counts[item.asInt64()] > hub_item_limit(item.asInt64()))
                        throw std::runtime_error("Unknown local collectible requirement");
                const auto &alternatives = local_sources[id]["alternate_requirements"];
                if (!alternatives.isNull() && (!alternatives.isArray() || alternatives.size() > 2))
                    throw std::runtime_error("Invalid local collectible routes");
                for (const auto &route : alternatives)
                    item_counts(route, valid_items);
                // reachable vanilla pickups do not activate platforms until actually collected
                const auto item = local_sources[id]["item"].asInt64();
                if (ingame::colored_gem_item(item))
                    result.local_reward_items.emplace(address, item);
            }
        }
        catch (const std::exception &e)
        {
            result.error = e.what();
        }
    }
    const bool local_rules_valid = result.error.empty();
    for (const auto &[key, id] : checks)
    {
        std::map<std::int64_t, unsigned> required;
        bool known = rules_valid && local_rules_valid && alternatives_valid;
        if (rules_valid)
        {
            const auto &rule = rules[std::to_string(id)];
            known = known && rule.isArray() && rule.size() <= 40;
            if (known)
                for (const auto &item : rule)
                {
                    if (!item.isInt64() || !valid_items.count(item.asInt64()) ||
                        ++required[item.asInt64()] > hub_item_limit(item.asInt64()))
                    {
                        known = false;
                        break;
                    }
                }
        }
        else
            known = false;
        const auto *level = level_logic(key.first);
        std::vector<std::map<std::int64_t, unsigned>> alternatives;
        if (alternate_rules.isObject() && alternate_rules.isMember(std::to_string(id)))
            try
            {
                const auto &routes = alternate_rules[std::to_string(id)];
                if (!routes.isArray() || routes.size() > 2)
                    throw std::runtime_error("Invalid alternate check routes");
                for (const auto &route : routes)
                    alternatives.push_back(item_counts(route, valid_items));
            }
            catch (const std::exception &error)
            {
                known = false;
                result.error = error.what();
            }
        result.checks.push_back({key, id, level ? level->title : key.first, std::move(required), known, std::move(alternatives)});
        if (!known && result.error.empty())
            result.error = "Unknown check requirements. Affected reachable markers hidden";
    }
    return result;
}

unsigned requirement_progress(const CheckRules &rules, const std::string &kind,
                              const std::vector<std::pair<std::int64_t, int>> &items,
                              const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending)
{
    if (kind == "token_hunt")
        return static_cast<unsigned>(std::count_if(items.begin(), items.end(), [](const auto &item) {
            return item.first == 0xcb0000 + 2203;
        }));
    return static_cast<unsigned>(std::count_if(rules.regular_clears.begin(), rules.regular_clears.end(), [&](auto id) {
        return checked.count(id) || pending.count(id);
    }));
}
bool cortex_requirements_met(const CheckRules &rules, const std::vector<std::pair<std::int64_t, int>> &items,
                             const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending)
{
    return std::all_of(rules.cortex_requirements.begin(), rules.cortex_requirements.end(), [&](const auto &entry) {
        return requirement_progress(rules, entry.first, items, checked, pending) >= entry.second;
    });
}

CheckProgress check_progress(const CheckRules &rules, const std::vector<std::pair<std::int64_t, int>> &items,
                             const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending,
                             const std::set<std::int64_t> &local_collected)
{
    CheckProgress result;
    result.error = rules.error;
    std::map<std::int64_t, unsigned> owned;
    for (const auto &item : items)
        ++owned[item.first];
    for (const auto &[location, item] : rules.local_reward_items)
        if (local_collected.count(location))
            owned[item] = std::max(owned[item], 1u);
    for (const auto &rule : rules.checks)
    {
        const auto &key = rule.key;
        const auto id = rule.id;
        const bool known = rule.known;
        const auto &required = rule.required;
        auto &count = result.levels[key.first];
        const auto event = key.second;
        auto *collectible = (event == "gem" || event == "extra_gem") ? &count.collectibles[0]
                            : event == "crystal"                     ? &count.collectibles[1]
                            : event == "relic"                       ? &count.collectibles[2]
                                                                     : nullptr;
        count.title = rule.title;
        ++count.total;
        if (collectible)
            ++collectible->total;
        const bool done = checked.count(id) || pending.count(id);
        if (done)
            ++count.done;
        if (done && collectible)
            ++collectible->done;
        if (!known)
            count.logic_known = false;
        if (!known && collectible)
            collectible->logic_known = false;
        if (!known && result.error.empty())
            result.error = "Unknown check requirements. Affected reachable markers hidden";
        const auto bosses = rules.boss_requirements.find(key.first);
        const bool bosses_cleared = bosses == rules.boss_requirements.end() ||
                                   std::all_of(bosses->second.begin(), bosses->second.end(),
                                               [&](auto boss) { return checked.count(boss) || pending.count(boss); });
        const bool final_ready = !rules.gated_finals.count(key.first) || cortex_requirements_met(rules, items, checked, pending);
        const auto satisfied = [&](const auto &needed) {
            return std::all_of(needed.begin(), needed.end(), [&](const auto &item) { return owned[item.first] >= item.second; });
        };
        if (known && bosses_cleared && final_ready && (satisfied(required) || std::any_of(rule.alternatives.begin(), rule.alternatives.end(), satisfied)))
        {
            ++count.in_logic;
            if (collectible)
                ++collectible->in_logic;
            if (!done)
                result.reachable.insert(id);
        }
    }
    return result;
}

std::set<std::int64_t> box_marker_ids(const CheckProgress &progress,
                                      const std::map<std::pair<std::string, std::string>, std::int64_t> &checks,
                                      const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending,
                                      bool out_of_logic)
{
    if (!out_of_logic)
        return progress.reachable;
    std::set<std::int64_t> result;
    for (const auto &[key, id] : checks)
        if (key.second.rfind("box:", 0) == 0 && !checked.count(id) && !pending.count(id))
            result.insert(id);
    return result;
}
