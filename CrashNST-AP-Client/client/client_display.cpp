#include "client_runtime.h"
#include "ap_console.h"
#include "game_runtime.h"
#include <algorithm>
#include <iostream>
#include "crate_catalogue.h"
static_assert(std::size(box_specs) <= ingame::max_boxes, "Crate catalogue exceeds bridge capacity");
void ClientRuntime::print_console(const std::string &text, bool popup)
{
    std::cout << text << std::endl;
    for (auto line : console_lines(text))
    {
        feed_history.push_back(line);
        if (feed_history.size() > ingame::max_lines)
            feed_history.pop_front();
        if (box_markers)
            box_markers->feed(line, popup && popups_ready);
    }
}

void ClientRuntime::update_bridge(AP_ConnectionStatus status)
{
    if (cloud && !session_failed)
        try
        {
            if (seed && cloud->ready())
            {
                std::array<unsigned, 3> areas{};
                const auto items = session.items();
                for (unsigned game = 1; game <= 3; ++game)
                    for (unsigned area = 1; area <= (game == 1 ? 3u : 5u); ++area)
                        if (hub_area_unlocked(*seed, items, game, area))
                            areas[game - 1] |= 1u << (area - 1);
                ingame::configure_hub_access(areas);
            }
            if (seed && cloud->ready() && !cloud->game_pid())
            {
                // the save gate waits after executable unpacking and before the first hub loads
                ingame::preserve_shared_ui_audio();
                ingame::configure_movement(seed->movement_styles);
            }
            cloud->tick(status == AP_ConnectionStatus::Authenticated);
        }
        catch (const std::exception &error)
        {
            session_error = error.what();
            session_failed = true;
        }
    if (box_markers && box_markers->startup() && standalone::save_ready())
    {
        box_markers->stop(true);
        box_markers.reset();
        render_announced = false;
        next_marker_attach = std::chrono::steady_clock::now();
    }
    if (!box_markers && std::chrono::steady_clock::now() >= next_marker_attach)
    {
        try
        {
            box_markers = std::make_unique<BoxMarkers>(pid, !standalone::save_ready(), box_marker_mode);
            box_markers->hub_distance(hub_label_distances);
            box_markers->tint_opacity(tint_opacity);
            names_loaded = false;
            gem_refresh.reset();
            for (const auto &line : feed_history)
                box_markers->feed(line, false);
            print_console(box_markers->startup() ? "AP startup console ready. F1 opens it. /server host:port, /slot "
                                                   "name, /connect correct the connection. /quit closes NST."
                                                 : "AP in-game adapter loaded. Waiting for NST's render frame.",
                          false);
            marker_error.clear();
        }
        catch (const std::exception &e)
        {
            if (marker_error != e.what())
                std::cerr << "In-game renderer: " << e.what() << std::endl;
            marker_error = e.what();
            next_marker_attach = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
    }
    std::deque<std::string> notices;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        notices.swap(console_notifications);
    }
    for (const auto &text : notices)
        print_console(text);
    bool messages_drained = false;
    auto messages = ap_messages(session.ready() ? session.slot() : -1, &messages_drained);
    for (const auto &item : fresh_receipts)
    {
        const auto sender = item["player"].asInt();
        const auto alias = player_aliases.count(sender) ? player_aliases.at(sender)
                           : sender                     ? "Player " + std::to_string(sender)
                                                        : "Server";
        auto received = ap_received_message(item["item"].asInt64(), item["flags"].asInt(), sender, alias,
                                            item.get("location_name", "").asString(), session.slot());
        messages.insert(messages.end(), received.begin(), received.end());
    }
    fresh_receipts.clear();
    for (const auto &line : messages)
    {
        std::string text;
        for (unsigned i = 0; i < line.count; ++i)
            text += line.spans[i].text;
        std::cout << text << std::endl;
        feed_history.push_back(line);
        if (feed_history.size() > ingame::max_lines)
            feed_history.pop_front();
        if (box_markers)
            box_markers->feed(line, popups_ready);
    }
    if (messages_drained && status == AP_ConnectionStatus::Authenticated && receipt_history_ready && !session_failed &&
        cloud && cloud->ready() && AP_GetDataPackageStatus() == AP_DataPackageSyncStatus::Synced)
        popups_ready = true;
    if (box_markers)
    {
        if (box_markers->startup())
            box_markers->startup_popup(startup_connection.show || session_failed);
        if (!render_announced && box_markers->rendering())
        {
            print_console(box_markers->startup()
                              ? "AP startup screen active. Correct the connection here. /quit closes NST."
                              : "AP in-game renderer active. F1: console | F2: entrances | F7: markers/HUD | F10: vanilla controls.",
                          false);
            render_announced = true;
        }
        box_markers->connection((status == AP_ConnectionStatus::Authenticated       ? "Connected"
                                 : status == AP_ConnectionStatus::ConnectionRefused ? "Connection refused"
                                                                                    : "Disconnected / connecting") +
                                std::string(" | ") + server + " | " + slot);
        if (box_markers->startup() && status == AP_ConnectionStatus::ConnectionRefused && status != previous)
            print_console("Connection refused: " + AP_GetConnectionError() +
                          ". Check /slot and /password, then /connect.");
        if (!names_loaded && AP_GetDataPackageStatus() == AP_DataPackageSyncStatus::Synced)
            try
            {
                auto names = ap_completion_names();
                if (session.ready())
                {
                    names.second.clear();
                    for (const auto &id : session.slot_data()["checks"].getMemberNames())
                        names.second.push_back(AP_GetLocationName(std::stoll(id)));
                }
                box_markers->completion(names.first, names.second);
                names_loaded = !names.first.empty();
                completion_error.clear();
            }
            catch (const std::exception &e)
            {
                if (completion_error != e.what())
                    std::cerr << "AP completion: " << e.what() << std::endl;
                completion_error = e.what();
            }
    }
}

void ClientRuntime::update_display(ClientTick &tick)
{
    if (tick.room_ready && box_markers)
    {
        std::set<std::int64_t> earned;
        for (const auto &observed : box_markers->collectible_observations())
        {
            const auto spec = ingame::collectible_spec(observed.key);
            if (!spec || !tick.levels.count(spec->key))
                continue;
            for (unsigned i = 0; i < 4; ++i)
                if (spec->items[i] && (observed.mask & spec->masks[i]))
                {
                    const auto id = ingame::collectible_location(*spec, i);
                    const auto found = tick.checks.find({spec->key, ingame::collectible_events[i]});
                    if (found != tick.checks.end() && found->second == id)
                    {
                        earned.insert(id);
                    }
                    else if (session.slot_data()["local_reward_sources"].isMember(std::to_string(id)))
                        session.observe_collectible(id);
                }
        }
        for (auto id : session.queue(earned))
            std::cout << "AP collectible check earned: " << id << '\n';
    }
    std::string gameplay_state;
    if (!session.ready())
        gameplay_state = "Gameplay waiting: AP session is not authenticated yet.";
    else if (!session_error.empty())
        gameplay_state = "Gameplay waiting: " + session_error;
    else if (!server_matches_session)
        gameplay_state = "Gameplay waiting: server seed has not matched the cached session.";
    else if (!cloud || !cloud->ready())
        gameplay_state = "Gameplay waiting: AP DataStorage snapshot is not ready.";
    else if (!tick.room_ready)
        gameplay_state =
            "Gameplay waiting: " +
            (routing_error.empty() ? std::string("discovering the level-routing hook.") : routing_error);
    else
    {
        gameplay_state = "Gameplay save/routing ready:";
        for (const auto &[campaign, file] : bound_saves)
            gameplay_state += " C" + std::string(1, campaign) + "=" + file;
    }
    if (gameplay_state != previous_gameplay_state)
    {
        std::cout << gameplay_state << std::endl;
        previous_gameplay_state = gameplay_state;
    }

    tick.box_checks = tick.room_ready && std::any_of(tick.checks.begin(), tick.checks.end(), [](const auto &entry) {
                          return entry.first.second.rfind("box:", 0) == 0;
                      });
    const auto pending = session.pending();
    const auto local_collected = session.local_collectibles();
    CheckProgress progress;
    if (tick.room_valid && box_markers)
    {
        progress = check_progress(seed->rules, tick.items, tick.checked, pending, local_collected);
        const auto progress_message =
            !progress.error.empty() ? progress.error : "Check display uses this seed's APWorld item rules.";
        if (progress_message != previous_progress_message)
        {
            std::cout << "AP check display: " << progress_message << std::endl;
            previous_progress_message = progress_message;
        }
        std::set<std::string> unlocked;
        for (const auto &[source, destination] : tick.placements)
            if (!level_locked(*seed, tick.items, source, tick.cleared))
                unlocked.insert(destination);
        for (const auto &[destination, source] : c2_secret_entrances)
        {
            const auto portal = "S" + std::string(destination).substr(1);
            if (!level_locked(*seed, tick.items, portal, tick.cleared))
                unlocked.insert(portal);
        }
        for (const auto *destination : {"L307", "L319"})
            if (!level_locked(*seed, tick.items, std::string(destination) + "-alternate", tick.cleared))
                unlocked.insert("S" + std::string(destination).substr(1));
        box_markers->progress(progress, tick.placements, unlocked);
    }
    if (tick.room_ready && box_markers)
    {
        std::vector<BoxMarker> markers;
        const auto marked = box_marker_ids(progress, tick.checks, tick.checked, pending, out_of_logic_markers);
        for (const auto &box : box_specs)
        {
            if (!marked.count(box.id))
                continue;
            BoxMarker marker;
            marker.level = box.level;
            marker.name = box.name;
            std::memcpy(marker.position.data(), box.position.data(), 12);
            marker.color =
                box_marker_color(session.slot_data()["box_item_spoilers"] == true, session.location_flags(), box.id);
            markers.push_back(marker);
        }
        box_markers->update(std::move(markers));
        std::string text = "F1: console | F2: entrances | F7: markers/HUD";
        const bool death_link = session.slot_data()["death_link"].asBool();
        box_markers->configure(seed->movement_styles, death_link);
        auto collectible_checked = tick.checked;
        collectible_checked.insert(pending.begin(), pending.end());
        const auto awards = box_markers->collectibles(ingame::collectible_configuration(
            session.slot_data(), tick.levels, tick.items, local_collected, collectible_checked));
        if (awards != last_collectible_awards)
        {
            last_collectible_awards = awards;
            std::cout << "AP collectible rewards applied: " << awards << " awards this attachment.\n";
        }
        const auto deaths = box_markers->deaths();
        if (deaths != last_deaths)
        {
            if (death_link && tick.online && deaths > last_deaths &&
                ingame::deathlink_due(deathlink_skipped, deathlink_amnesty, deaths - last_deaths))
            {
                AP_DeathLinkSend();
                print_console("DeathLink sent.");
            }
            last_deaths = deaths;
        }
        box_markers->message(text);
    }
    else if (box_markers)
    {
        box_markers->update({});
        box_markers->configure({}, false);
        box_markers->collectibles({});
        box_markers->message(gameplay_state);
    }
}
