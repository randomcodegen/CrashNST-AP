#include "client_runtime.h"
#include <algorithm>
#include <iostream>
void ClientRuntime::update_c2(ClientTick &tick)
{
    if (gate_discovery.valid() && gate_discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            const auto found = gate_discovery.get();
            if (tick.room_ready && tick.c2_active && gate_epoch == zone_epoch)
            {
                if (found.empty())
                    throw std::runtime_error("Crash 2 hub objects are not loaded");
                for (const auto &[level, entity] : found)
                {
                    try
                    {
                        if (level[0] == 'F')
                        {
                            const auto current = c2_lifts.find(level);
                            if (current != c2_lifts.end() && current->second->entity() == entity &&
                                !gate_errors.count(level))
                                continue;
                            c2_lifts[level] = std::make_unique<C2LiftTraversal>(pid, level[6] - '0', entity);
                        }
                        else
                        {
                            const auto current = c2_gates.find(level);
                            if (current != c2_gates.end() && current->second->entity() == entity &&
                                !gate_errors.count(level))
                                continue;
                            c2_gates[level] = std::make_unique<C2PortalGate>(pid, level, entity);
                        }
                        gate_errors.erase(level);
                        std::cout << "Crash 2 adapter attached: " << level << ", PID " << pid << std::endl;
                    }
                    catch (const std::exception &error)
                    {
                        if (gate_errors[level] != error.what())
                            std::cerr << level << " attach: " << error.what() << std::endl;
                        gate_errors[level] = error.what();
                    }
                }
                previous_gate_error.clear();
            }
        }
        catch (const std::exception &error)
        {
            if (tick.c2_active && gate_epoch == zone_epoch)
            {
                if (previous_gate_error != error.what())
                    std::cerr << "Crash 2 gate attach: " << error.what() << std::endl;
                previous_gate_error = error.what();
            }
        }
        next_gate_probe =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(gate_epoch == zone_epoch ? 250 : 0);
    }
    if (tick.room_ready && tick.c2_active && !gate_discovery.valid() &&
        (rediscover_c2 || c2_gates.size() < c2_regular_levels.size() + 5 || c2_lifts.size() < 5 ||
         !gate_errors.empty()) &&
        std::chrono::steady_clock::now() >= next_gate_probe)
    {
        gate_epoch = zone_epoch;
        rediscover_c2 = false;
        gate_discovery = std::async(std::launch::async, [pid = pid] { return find_c2_hub_entities(pid); });
        std::cout << "Discovering loaded Crash 2 hub objects..." << std::endl;
    }
    if (tick.c2_active)
        for (auto &[level, gate] : c2_gates)
        {
            try
            {
                const bool locked = level[0] == 'B' ? !tick.room_ready || level_locked(*seed, tick.items, level, tick.cleared)
                                                    : tick.room_ready && level_locked(*seed, tick.items, level, tick.cleared);
                gate->set_locked(locked);
                gate_errors.erase(level);
            }
            catch (const std::exception &error)
            {
                if (gate_errors[level] != error.what())
                    std::cerr << "Crash 2 gate update: " << error.what() << std::endl;
                gate_errors[level] = error.what();
            }
        }
    if (tick.c2_active)
        try
        {
            std::map<std::string, std::uintptr_t> active_bosses;
            if (tick.room_ready)
            {
                for (const auto &[level, gate] : c2_gates)
                    if (level[0] == 'B' && !level_locked(*seed, tick.items, level, tick.cleared) && !gate_errors.count(level) &&
                        gate->state().hub_active)
                        active_bosses.emplace(level, gate->entity());
            }
            if (c2_boss && (!c2_boss->matches(active_bosses) || !c2_boss->update()))
                c2_boss.reset();
            if (!c2_boss && !active_bosses.empty())
            {
                c2_boss = std::make_unique<C2BossPortals>(pid, active_bosses);
                for (const auto &[level, entity] : active_bosses)
                    std::cout << level
                              << " unlocked. Approach the boss entrance once to enable it, then step away "
                              << "and approach again after its teleport effect appears.\n"
                              << std::flush;
            }
            gate_errors.erase("Boss events");
        }
        catch (const std::exception &error)
        {
            c2_boss.reset();
            if (gate_errors["Boss events"] != error.what())
                std::cerr << "Crash 2 boss events: " << error.what() << std::endl;
            gate_errors["Boss events"] = error.what();
        }
    const bool c2_in_room =
        std::any_of(tick.levels.begin(), tick.levels.end(), [](const auto &level) { return level[1] == '2'; });
    if (tick.c2_active)
        for (auto &[name, lift] : c2_lifts)
        {
            try
            {
                lift->set_enabled(tick.room_ready && c2_in_room);
                gate_errors.erase(name);
            }
            catch (const std::exception &error)
            {
                if (gate_errors[name] != error.what())
                    std::cerr << name << " update: " << error.what() << std::endl;
                gate_errors[name] = error.what();
            }
        }
}

C3HubState ClientRuntime::update_c3_gates(ClientTick &tick)
{
    if (c3_discovery.valid() && c3_discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            const auto found = c3_discovery.get();
            if (tick.room_ready && tick.c3_active && c3_epoch == zone_epoch)
            {
                if (found.entities.empty())
                    throw std::runtime_error("Crash 3 hub buttons are not active");
                for (const auto &[key, entity] : found.entities)
                {
                    const auto current = c3_gates.find(key);
                    if (current != c3_gates.end() && current->second->entity() == entity && !c3_errors.count(key))
                        continue;
                    c3_gates[key] = std::make_unique<PortalTriggerGate>(pid, key, entity, found.types);
                    c3_previous_locked.erase(key);
                    c3_errors.erase(key);
                    std::cout << "Crash 3 button gate attached: " << key << ", PID " << pid << std::endl;
                }
                c3_errors.erase("Discovery");
            }
        }
        catch (const std::exception &error)
        {
            if (tick.c3_active && c3_epoch == zone_epoch)
            {
                if (c3_errors["Discovery"] != error.what())
                    std::cerr << "Crash 3 button discovery: " << error.what() << std::endl;
                c3_errors["Discovery"] = error.what();
            }
        }
        next_c3_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(c3_epoch == zone_epoch ? 250 : 0);
    }
    if (tick.room_ready && tick.c3_active && !c3_discovery.valid() &&
        (rediscover_c3 || c3_gates.size() < 36 || !c3_errors.empty()) &&
        std::chrono::steady_clock::now() >= next_c3_probe)
    {
        c3_epoch = zone_epoch;
        rediscover_c3 = false;
        c3_discovery = std::async(std::launch::async, [pid = pid] { return find_c3_buttons(pid); });
        std::cout << "Discovering loaded Crash 3 hub buttons..." << std::endl;
    }
    bool c3_hub_active = tick.c3_active && !c3_gates.empty();
    std::set<std::string> c3_allowed;
    std::map<std::string, std::uintptr_t> c3_entities;
    if (tick.room_ready)
        for (const auto &[key, gate] : c3_gates)
            if (!level_locked(*seed, tick.items, key.substr(0, 4), tick.cleared))
                c3_allowed.insert(key);
    if (tick.c3_active)
        for (auto &[key, gate] : c3_gates)
        {
            try
            {
                // alternate buttons follow their destination's lock settings
                bool locked = !c3_allowed.count(key);
                if (gate->set_locked(locked))
                {
                    c3_entities.emplace(key, gate->entity());
                    if (!c3_previous_locked.count(key) || c3_previous_locked.at(key) != locked)
                        std::cout << "Crash 3 " << key << ": button events " << (locked ? "blocked" : "restored")
                                  << ". Button activation requested.\n"
                                  << std::flush;
                    c3_previous_locked[key] = locked;
                }
                else
                {
                    c3_previous_locked.erase(key);
                    c3_hub_active = false;
                }
                c3_errors.erase(key);
            }
            catch (const std::exception &error)
            {
                if (c3_errors[key] != error.what())
                    std::cerr << "Crash 3 " << key << " gate: " << error.what() << std::endl;
                c3_errors[key] = error.what();
                c3_hub_active = false;
            }
        }

    return {c3_hub_active, std::move(c3_allowed), std::move(c3_entities)};
}

void ClientRuntime::update_c1(ClientTick &tick)
{
    if (c1_discovery.valid() && c1_discovery.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        try
        {
            const auto found = c1_discovery.get();
            if (tick.room_ready && tick.c1_active && c1_epoch == zone_epoch && found.anchor && found.manager_meta)
            {
                c1_map = std::make_unique<C1MapAccess>(pid, found);
                previous_c1_allowed.clear();
                std::cout << "C1 map access hook attached. Level confirmation follows AP lock settings.\n" << std::flush;
            }
            c1_error.clear();
        }
        catch (const std::exception &error)
        {
            if (tick.c1_active && c1_epoch == zone_epoch)
            {
                if (c1_error != error.what())
                    std::cerr << "C1 map: " << error.what() << std::endl;
                c1_error = error.what();
            }
        }
        next_c1_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(c1_epoch == zone_epoch ? 250 : 0);
    }
    if (tick.room_ready && tick.c1_active && !c1_map && !c1_discovery.valid() &&
        std::chrono::steady_clock::now() >= next_c1_probe)
    {
        c1_epoch = zone_epoch;
        c1_discovery = std::async(std::launch::async, [pid = pid] { return find_c1_map(pid); });
    }
    if (c1_map && tick.c1_active)
    {
        try
        {
            std::set<std::string> allowed;
            bool traversal = false;
            if (tick.room_ready)
                for (const auto &[key, destination] : tick.placements)
                {
                    if (key.size() != 4 || key[1] != '1')
                        continue;
                    traversal = true;
                    if (!level_locked(*seed, tick.items, key, tick.cleared))
                        allowed.insert(key);
                }
            if (!c1_map->update(allowed, traversal))
                c1_map.reset();
            if (allowed != previous_c1_allowed)
            {
                std::cout << "Crash 1 map: " << allowed.size() << " entrances permitted. Map traversal "
                          << (traversal ? "enabled" : "restored") << std::endl;
                previous_c1_allowed = allowed;
            }
            c1_error.clear();
        }
        catch (const std::exception &error)
        {
            if (c1_error != error.what())
                std::cerr << "C1 map: " << error.what() << std::endl;
            c1_error = error.what();
            c1_map.reset();
            next_c1_probe = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
    }
}

void ClientRuntime::update_c3_access(ClientTick &tick, const C3HubState &hub)
{
    const auto &[c3_hub_active, c3_allowed, c3_entities] = hub;
    if (c3_hub_active)
    {
        try
        {
            if (!c3_portals && c3_entities.size() == 36 && tick.room_ready &&
                std::chrono::steady_clock::now() >= next_c3_portal_attach)
            {
                const auto &anchor = *c3_entities.begin();
                c3_portals = std::make_unique<C3PortalAccess>(pid, anchor.second, anchor.first);
                std::cout << "C3 portal entry hook attached. Destinations follow AP lock settings.\n"
                          << std::flush;
            }
            if (c3_portals && !c3_portals->update(c3_allowed))
                c3_portals.reset();
            c3_errors.erase("Portal entry");
        }
        catch (const std::exception &error)
        {
            if (c3_errors["Portal entry"] != error.what())
                std::cerr << "C3 portal entry: " << error.what() << std::endl;
            c3_errors["Portal entry"] = error.what();
            c3_portals.reset();
            next_c3_portal_attach = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
    }
    if (c3_hub_active)
    {
        try
        {
            if (c3_access && !c3_access->matches(c3_entities))
                c3_access.reset();
            const bool c3_selected =
                tick.room_ready && std::any_of(tick.levels.begin(), tick.levels.end(),
                                               [](const auto &level) { return level.size() == 4 && level[1] == '3'; });
            if (!c3_access && c3_gates.size() == 36 && c3_selected &&
                std::chrono::steady_clock::now() >= next_c3_access_attach)
            {
                c3_access = std::make_unique<C3HubAccess>(pid, c3_entities);
                std::cout << "C3 activation scripts attached. Barrier contact opens hub paths.\n" << std::flush;
            }
            std::array<bool, 4> areas{};
            for (unsigned i = 0; i < areas.size(); ++i)
                areas[i] = tick.room_ready && hub_area_unlocked(*seed, tick.items, 3, i + 2);
            if (c3_access && !c3_access->update(c3_allowed, c3_selected, areas))
                c3_access.reset();
            if (box_markers)
                box_markers->c3_buttons(c3_access != nullptr, c3_allowed);
            c3_errors.erase("Hub access");
        }
        catch (const std::exception &error)
        {
            if (box_markers)
                box_markers->c3_buttons(false);
            if (c3_errors["Hub access"] != error.what())
                std::cerr << "C3 hub access: " << error.what() << std::endl;
            c3_errors["Hub access"] = error.what();
            c3_access.reset();
            next_c3_access_attach = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        }
    }
    else if (box_markers)
        box_markers->c3_buttons(false);
}
