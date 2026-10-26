#pragma once
#include "Archipelago.h"
#include "cloud_saves.h"
#include "console.h"
#include "game_hooks.h"
#include "ingame_bridge.h"
#include "seed_config.h"
#include "session.h"
#include "standalone.h"
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct ClientTick
{
    AP_ConnectionStatus status = AP_ConnectionStatus::Disconnected;
    std::vector<std::pair<std::int64_t, int>> items;
    std::set<std::int64_t> checked;
    const std::map<CheckKey, std::int64_t> &checks;
    const std::set<std::string> &levels;
    const std::map<std::string, std::string> &placements;
    bool online, room_valid = false, room_ready = false, box_checks = false, seed_matches = false;
    bool c1_active = false, c2_active = false, c3_active = false;
    std::set<std::int64_t> cleared;
};
struct C3HubState
{
    bool active;
    std::set<std::string> allowed;
    std::map<std::string, std::uintptr_t> entities;
};

class ClientRuntime
{
  public:
    ClientRuntime(std::string server, std::string slot) : server(std::move(server)), slot(std::move(slot))
    {
    }
    ~ClientRuntime()
    {
        AP_Shutdown();
    }
    int run();

  private:
    std::string server, slot;
    const DWORD pid = GetCurrentProcessId();
    bool running = true;
    std::mutex state_mutex;
    std::deque<std::string> ap_packets;
    std::size_t ap_packet_bytes = 0;
    std::string ap_packet_error;
    std::deque<std::string> console_notifications;
    const std::vector<std::string> c2_regular_levels = c2_portal_levels();
    std::filesystem::path save_dir;
    std::map<char, std::string> bound_saves;
    const std::filesystem::path session_directory = standalone::state_directory() / L"sessions";
    Session session;
    std::optional<SeedConfig> seed;
    std::unique_ptr<CloudSaves> cloud;
    std::string observed_seed, session_error;
    bool server_matches_session = false, session_failed = false;
    std::string session_profile = server + '\n' + slot;

    std::string staged_server = server, staged_slot = slot, connection_password;
    std::vector<std::string> c2_access_levels = c2_regular_levels;
    ingame::StartupConnection startup_connection;
    std::chrono::steady_clock::time_point transport_connected_at{};
    bool popups_ready = false, receipt_history_ready = false;
    std::map<int, std::string> player_aliases;
    std::vector<Json::Value> fresh_receipts;
    std::vector<Json::Value> deferred_receipts;
    std::size_t deferred_receipt_bytes = 0;

    AP_ConnectionStatus previous = AP_ConnectionStatus::Disconnected;
    std::size_t previous_items = 0, previous_checks = 0;
    bool first = true;
    std::map<std::string, save_probe::Zones> previous_zones;
    std::map<std::string, std::string> previous_save_errors;
    std::map<std::string, std::set<std::int64_t>> displayed_candidates;
    std::string previous_progress_message;
    std::map<std::string, bool> previous_access;
    std::map<std::string, std::unique_ptr<C2PortalGate>> c2_gates;
    std::map<std::string, std::unique_ptr<C2LiftTraversal>> c2_lifts;
    std::unique_ptr<C2BossPortals> c2_boss;
    std::map<std::string, std::unique_ptr<PortalTriggerGate>> c3_gates;
    std::unique_ptr<C3HubAccess> c3_access;
    std::unique_ptr<C3PortalAccess> c3_portals;
    std::unique_ptr<C1MapAccess> c1_map;
    std::future<C1MapDiscovery> c1_discovery;
    std::chrono::steady_clock::time_point next_c1_probe = std::chrono::steady_clock::now();
    std::string c1_error;
    std::unique_ptr<AbilityAccess> ability_access;
    AbilityOwnership previous_abilities{};
    bool ability_refresh_pending = false;
    std::string ability_error;
    std::chrono::steady_clock::time_point next_ability_probe = std::chrono::steady_clock::now();
    GemDiscovery gem_cache;
    std::unique_ptr<CrateEvents> crate_events;
    CrateMetadataPointer crate_pointer;
    std::unique_ptr<BoxMarkers> box_markers;
    const std::filesystem::path display_file = standalone::state_directory() / "display.json";
    unsigned box_marker_mode = ingame::MarkersTint;
    bool out_of_logic_markers = false;
    unsigned deathlink_amnesty = 0, deathlink_skipped = 0;
    std::array<unsigned, 3> hub_label_distances = ingame::default_hub_distances;
    float tint_opacity = ingame::default_tint_opacity;

    std::string marker_error;
    std::chrono::steady_clock::time_point next_marker_attach = std::chrono::steady_clock::now();
    std::deque<ingame::Line> feed_history;
    std::deque<std::string> console_commands;
    bool names_loaded = false, render_announced = false;
    std::string completion_error;
    unsigned last_deaths = 0, last_collectible_awards = 0;
    std::unique_ptr<LevelRouting> level_routing;
    unsigned routing_start_count = 0, routing_secret_return_count = 0;
    std::string routing_error;
    std::string crate_error;
    std::set<std::int64_t> crate_checks;
    std::chrono::steady_clock::time_point next_crate_probe = std::chrono::steady_clock::now();
    std::future<GemDiscovery> gem_discovery;
    std::map<std::string, std::unique_ptr<GemRouteAccess>> gem_access;
    std::unique_ptr<GemRefreshAccess> gem_refresh;
    bool gem_refresh_pending = false;
    std::string gem_error;
    std::chrono::steady_clock::time_point next_gem_probe = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_gem_bootstrap = std::chrono::steady_clock::now();
    GemOwnership previous_gems{};
    std::set<std::string> previous_c1_allowed;
    std::chrono::steady_clock::time_point next_c3_access_attach = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point next_c3_portal_attach = std::chrono::steady_clock::now();
    std::future<C3ButtonDiscovery> c3_discovery;
    std::chrono::steady_clock::time_point next_c3_probe = std::chrono::steady_clock::now();
    std::map<std::string, std::string> c3_errors;
    std::map<std::string, bool> c3_previous_locked;
    std::chrono::steady_clock::time_point next_gate_probe = std::chrono::steady_clock::now();
    std::uintptr_t zone_base = 0;
    ActiveZone previous_zone;
    std::uint64_t zone_epoch = 0, gate_epoch = 0, c1_epoch = 0, c3_epoch = 0;
    bool rediscover_c2 = true, rediscover_c3 = true;
    std::string zone_error;
    std::string previous_gate_error;
    std::map<std::string, std::string> gate_errors;
    std::future<std::map<std::string, std::uintptr_t>> gate_discovery;
    std::string previous_seed_error;
    std::set<std::int64_t> submitted_checks;
    bool goal_sent = false;
    std::string previous_gameplay_state;
    std::map<char, std::string> previous_goal_progress;

    void queue_ap_packet(std::string raw);
    void register_packet_callbacks();
    void receive_items(const Json::Value &message);
    void initialize_ap();
    void save_connection();
    void print_console(const std::string &text, bool popup = true);
    int initialize();
    void handle_commands();
    void process_packets(AP_ConnectionStatus status);
    void update_bridge(AP_ConnectionStatus status);
    void update_routing(ClientTick &tick);
    void update_display(ClientTick &tick);
    void update_saves(ClientTick &tick);
    void update_zone(ClientTick &tick);
    void update_c2(ClientTick &tick);
    C3HubState update_c3_gates(ClientTick &tick);
    void update_c1(ClientTick &tick);
    void update_c3_access(ClientTick &tick, const C3HubState &hub);
    void update_ownership(ClientTick &tick);
    void log_status(ClientTick &tick);
    void wait_for_commands();
    void drain_crates(ClientTick &tick);
    void finish();
};
