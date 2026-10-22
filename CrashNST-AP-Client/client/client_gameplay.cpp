#include "client_runtime.h"
#include <algorithm>
#include <iostream>
using save_probe::level_key;
void ClientRuntime::update_routing(ClientTick &tick)
{
    if (session.ready() && !session_failed)
    {
        try
        {
            if (cloud && cloud->ready())
                bound_saves = session.bind_saves(save_dir, tick.levels);
            session_error.clear();
        }
        catch (const std::exception &error)
        {
            session_error = error.what();
            bound_saves.clear();
        }
    }
    tick.room_valid = session.ready() && seed && !session_failed && server_matches_session && session_error.empty() &&
                      (cloud && cloud->ready() && cloud->game_pid() == pid);
    if (tick.room_valid && !level_routing)
    {
        try
        {
            level_routing = std::make_unique<LevelRouting>(pid, tick.placements);
            routing_start_count = 0;
            routing_secret_return_count = 0;
            gem_refresh.reset();
            std::cout << "Campaign-start routing attached. Continue restores remembered hub entrances.\n";
            routing_error.clear();
        }
        catch (const std::exception &e)
        {
            if (routing_error != e.what())
            {
                if (std::string(e.what()) == "Zone registry is not loaded")
                    std::cout << "Startup waiting: zone registry is not loaded yet.\n";
                else
                    std::cerr << "Entrance routing: " << e.what() << std::endl;
            }
            routing_error = e.what();
        }
    }
    if (tick.room_valid && level_routing)
        try
        {
            const auto last = level_routing->last_entrance();
            if (!last.empty())
                session.remember_entrance(last);
            auto entrances = session.hub_entrances();
            for (auto it = entrances.begin(); it != entrances.end();)
                if (level_locked(*seed, tick.items, it->second, tick.cleared))
                    it = entrances.erase(it);
                else
                    ++it;
            level_routing->resume_entrances(entrances);
            std::set<std::string> allowed;
            for (const auto &[destination, source] : secret_stages)
                if (tick.levels.count(destination) && !level_locked(*seed, tick.items, destination, tick.cleared))
                    allowed.insert(destination);
            level_routing->secrets(allowed);
            const auto count = level_routing->start_count();
            if (count != routing_start_count)
            {
                routing_start_count = count;
                std::cout << "Campaign start redirected to its hub.\n";
            }
            const auto secret_returns = level_routing->secret_return_count();
            if (secret_returns != routing_secret_return_count)
            {
                routing_secret_return_count = secret_returns;
                std::cout << "CB2 secret-room return selected. Original entrance preserved.\n";
            }
            if (gem_cache.secret_handle && level_routing->bind_secret(*gem_cache.secret_handle))
            {
                gem_refresh.reset();
                routing_error.clear();
                std::cout << "Entrance routing ready. Secret stages use their vanilla paths and Access items.\n";
            }
        }
        catch (const std::exception &e)
        {
            if (routing_error != e.what())
                std::cerr << "Entrance routing: " << e.what() << std::endl;
            routing_error = e.what();
        }
    if (!tick.room_valid)
        level_routing.reset();
    tick.room_ready = tick.room_valid && level_routing && level_routing->ready();
}

void ClientRuntime::update_zone(ClientTick &tick)
{
    if (tick.room_ready)
    {
        for (const auto &level : c2_access_levels)
        {
            const bool access = !level_locked(*seed, tick.items, level, tick.cleared);
            const auto previous_access_it = previous_access.find(level);
            if (previous_access_it == previous_access.end() || previous_access_it->second != access)
                std::cout << "Desired " << level << " access: " << (access ? "open" : "closed") << std::endl;
            previous_access[level] = access;
        }
    }
    else
    {
        previous_access.clear();
    }
    ActiveZone active_zone;
    if (tick.room_ready)
        try
        {
            if (!zone_base)
                zone_base = nst_module_base(GetCurrentProcess());
            active_zone = read_active_zone(GetCurrentProcess(), zone_base);
            zone_error.clear();
        }
        catch (const std::exception &e)
        {
            if (zone_error != e.what())
                std::cerr << "Active zone: " << e.what() << std::endl;
            zone_error = e.what();
        }
    if (!(active_zone == previous_zone))
    {
        previous_zone = active_zone;
        ++zone_epoch;
        rediscover_c2 = rediscover_c3 = true;
        next_gate_probe = next_c1_probe = next_c3_probe = std::chrono::steady_clock::now();
        next_c3_access_attach = next_c3_portal_attach = std::chrono::steady_clock::now();
    }
    const bool scene_ready = active_zone.campaign && active_zone.entity_context && active_zone.script_context;
    tick.c1_active = scene_ready && active_zone.hub == 1;
    tick.c2_active = scene_ready && active_zone.hub == 2;
    tick.c3_active = scene_ready && active_zone.hub == 3;
    if (box_markers && tick.online && AP_DeathLinkPending())
    {
        const bool apply =
            tick.room_ready && session.slot_data()["death_link"].asBool() && death_link_level(active_zone);
        if (apply)
            box_markers->receive_death();
        AP_DeathLinkClear();
        print_console(apply ? "DeathLink received." : "DeathLink ignored outside an active level.");
    }
}

void ClientRuntime::update_ownership(ClientTick &tick)
{
    AbilityOwnership desired_abilities{};
    GemOwnership desired_gems{};
    if (tick.room_ready)
    {
        std::set<std::int64_t> received;
        std::set<char> campaigns;
        for (const auto &item : tick.items)
            received.insert(item.first);
        for (auto id : session.local_collectibles())
        {
            const auto item = ingame::local_collectible_item(session.slot_data(), id);
            if (ingame::colored_gem_item(item))
                received.insert(item);
        }
        for (const auto &level : tick.levels)
            if (level.size() == 4)
                campaigns.insert(level[1]);
        desired_gems = gem_ownership(received, campaigns);
        desired_abilities = ability_ownership(received, campaigns);
    }
    const auto attach_abilities = [&](const GemDiscovery &found) {
        if (!found.power_meta ||
            (ability_access && ability_access->meta() == found.power_meta &&
             ability_access->menu_meta() == found.ability_menu_meta && ability_access->hud_meta() == found.hud_meta &&
             ability_access->jump_filter() == (box_markers ? box_markers->double_jump_filter() : 0)))
            return;
        if (ability_access && ability_access->filler_pending())
            std::cerr << "Reserved filler interrupted by loading. It will not replay.\n";
        ability_access.reset();
        try
        {
            ability_access = std::make_unique<AbilityAccess>(pid, found, desired_abilities,
                                                             box_markers ? box_markers->double_jump_filter() : 0);
            ability_error.clear();
            ability_refresh_pending = true;
            std::cout << "AP ability adapter attached. Ability-cache refresh queued.\n" << std::flush;
        }
        catch (const std::exception &error)
        {
            if (ability_error != error.what())
                std::cerr << "Ability attachment: " << error.what() << std::endl;
            ability_error = error.what();
        }
    };
    // filler needs HUD metadata
    if (tick.room_ready && (!ability_access || !ability_access->hud_meta()) &&
        std::chrono::steady_clock::now() >= next_ability_probe)
    {
        next_ability_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        try
        {
            attach_abilities(find_ability_metadata(pid));
        }
        catch (const std::exception &error)
        {
            if (ability_error != error.what())
                std::cerr << "Ability discovery: " << error.what() << std::endl;
            ability_error = error.what();
        }
    }
    // box checks shouldnt wait for full gem/ability handle discovery during level loading
    if (!crate_events && tick.box_checks && tick.room_ready && std::chrono::steady_clock::now() >= next_crate_probe)
    {
        next_crate_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        try
        {
            if (const auto meta = find_crate_metadata(pid, crate_pointer))
            {
                crate_events =
                    std::make_unique<CrateEvents>(pid, meta, box_markers ? box_markers->crate_identity() : 0);
                crate_error.clear();
            }
        }
        catch (const std::exception &error)
        {
            if (crate_error != error.what())
                std::cerr << "Box observer: " << error.what() << std::endl;
            crate_error = error.what();
        }
    }
    if (gem_discovery.valid() && gem_discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            const auto found = gem_discovery.get();
            if (tick.room_valid && found.registry_metadata && !gem_cache.registry_metadata)
                std::cout << "Engine metadata registry attached. Script discovery uses validated pointers.\n";
            if (tick.room_valid)
                gem_cache = found;
            if (tick.room_ready)
            {
                gem_cache = found;
                if (found.crate_meta && !crate_events && tick.box_checks)
                {
                    try
                    {
                        crate_events = std::make_unique<CrateEvents>(pid, found.crate_meta,
                                                                     box_markers ? box_markers->crate_identity() : 0);
                        crate_error.clear();
                    }
                    catch (const std::exception &error)
                    {
                        if (crate_error != error.what())
                            std::cerr << "Box observer: " << error.what() << std::endl;
                        crate_error = error.what();
                    }
                }
                attach_abilities(found);
                for (const auto &[name, meta] : found.graphs)
                {
                    const auto old = gem_access.find(name);
                    if (old != gem_access.end() && old->second->meta() == meta)
                        continue;
                    gem_refresh.reset();
                    gem_access.erase(name);
                    gem_access.emplace(name, std::make_unique<GemRouteAccess>(pid, found, name, desired_gems));
                    std::cout << "Gem route attached: " << name
                              << ". Ownership uses AP items for the active campaign.\n"
                              << std::flush;
                }
                gem_error.clear();
            }
        }
        catch (const std::exception &error)
        {
            if (gem_error != error.what())
            {
                if (std::string(error.what()) == "Campaign-check script graph is not loaded")
                    std::cout << "Gameplay waiting: campaign-check script graph is not loaded yet." << std::endl;
                else
                    std::cerr << "Gem discovery: " << error.what() << std::endl;
            }
            gem_error = error.what();
            gem_cache = {};
        }
        next_gem_probe = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
    if (tick.room_valid && !gem_discovery.valid() && std::chrono::steady_clock::now() >= next_gem_probe)
    {
        // periodically bootstrap again to cover script slabs allocated outside the cached allocator
        if (std::chrono::steady_clock::now() >= next_gem_bootstrap)
        {
            if (!gem_cache.registry_metadata)
                gem_cache = {};
            next_gem_bootstrap = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
        const auto cached = gem_cache;
        gem_discovery = std::async(std::launch::async, [pid = pid, cached] { return find_gem_routes(pid, cached); });
    }
    for (auto it = gem_access.begin(); it != gem_access.end();)
    {
        try
        {
            it->second->update(desired_gems);
            ++it;
        }
        catch (const std::exception &error)
        {
            std::cerr << "Gem route " << it->first << ": " << error.what() << std::endl;
            gem_refresh.reset();
            it = gem_access.erase(it);
            gem_cache = {};
        }
    }
    if (!gem_refresh && (!gem_access.empty() || level_routing) && gem_cache.power_meta && tick.room_ready)
    {
        try
        {
            std::vector<GemRouteProbe> jobs;
            for (const auto &[name, access] : gem_access)
                jobs.push_back(access->refresh_job());
            gem_refresh = std::make_unique<GemRefreshAccess>(pid, gem_cache, jobs, desired_gems,
                                                             box_markers ? box_markers->tick() : 0);
            gem_refresh_pending = true;
            std::cout << "AP gem game-thread refresh attached.\n" << std::flush;
        }
        catch (const std::exception &error)
        {
            if (gem_error != error.what())
                std::cerr << "Gem refresh attachment: " << error.what() << std::endl;
            gem_error = error.what();
        }
    }
    if (gem_refresh)
    {
        try
        {
            gem_refresh->update(desired_gems);
        }
        catch (const std::exception &error)
        {
            std::cerr << "Gem refresh: " << error.what() << std::endl;
            gem_refresh.reset();
            gem_cache = {};
        }
    }
    if (ability_access)
    {
        try
        {
            if (!ability_access->update(desired_abilities))
            {
                ability_access.reset();
                gem_cache = {};
                ability_refresh_pending = true;
                next_gem_probe = std::chrono::steady_clock::now();
                std::cout << "Ability graph changed during loading. Hooks detached, rediscovering.\n" << std::flush;
            }
        }
        catch (const std::exception &error)
        {
            std::cerr << "Ability access: " << error.what() << std::endl;
            ability_access.reset();
            gem_cache = {};
        }
    }
    if (previous_abilities != desired_abilities)
    {
        ability_refresh_pending = true;
        std::cout << "AP ability ownership updated. Ability-cache refresh queued.\n" << std::flush;
        previous_abilities = desired_abilities;
    }
    if (ability_access && ability_refresh_pending)
    {
        try
        {
            if (ability_access->refreshed())
            {
                std::cout << "AP ability-cache refresh applied on the game thread.\n" << std::flush;
                ability_refresh_pending = false;
            }
        }
        catch (const std::exception &error)
        {
            std::cerr << "Ability refresh acknowledgement: " << error.what() << std::endl;
        }
    }
    if (tick.room_ready && ability_access && !ability_refresh_pending)
    {
        try
        {
            for (unsigned index = session.filler_cursor(); index < tick.items.size(); ++index)
            {
                const auto item = tick.items[index].first;
                if (item != 0xCB0000 + 2201 && item != 0xCB0000 + 2202)
                    continue;
                if (!ability_access->filler_ready() || ability_access->filler_pending())
                    break;
                const auto scene = read_scene(GetCurrentProcess());
                const auto level = level_key(scene.zone);
                if (!scene.camera_active || !level_accessible(*seed, tick.items, level, tick.cleared))
                    break;
                if (session.reserve_filler(index))
                {
                    ability_access->queue_filler(index, item, scene.zone_address);
                    std::cout << "Filler queued on the game thread: "
                              << (item == 0xCB0000 + 2201 ? "1-Up"
                                                          : std::to_string(wumpa_amount(index)) + " Wumpa Fruit")
                              << " (AP index " << index << ").\n"
                              << std::flush;
                }
                break; // apply rewards one at a time
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Filler delivery: " << e.what() << std::endl;
        }
    }
    if (previous_gems != desired_gems)
    {
        gem_refresh_pending = true;
        std::cout << "AP colored-gem ownership updated. Live route refresh queued.\n" << std::flush;
        previous_gems = desired_gems;
    }
    if (gem_refresh && gem_refresh_pending)
    {
        try
        {
            if (gem_refresh->refreshed())
            {
                std::cout << "AP gem refresh acknowledged on the game thread: " << gem_refresh->applied()
                          << " active components refreshed (" << gem_refresh->diagnostics() << ").\n";
                if (!gem_refresh->applied())
                    std::cout << "No matching live gem component refreshed. Future components use game activation.\n";
                std::cout << std::flush;
                gem_refresh_pending = false;
            }
        }
        catch (const std::exception &error)
        {
            std::cerr << "Gem refresh acknowledgement: " << error.what() << std::endl;
        }
    }
}
