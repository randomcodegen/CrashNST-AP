#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

std::vector<unsigned char> movement_input_stub(std::uintptr_t blocked_controller);

struct ZoneRecord
{
    std::string key, path;
    std::uintptr_t address = 0;
};
struct ActiveZone
{
    std::string zone, key;
    unsigned campaign = 0, hub = 0;
    std::uintptr_t world = 0, zone_name = 0, info = 0, entity_context = 0, script_context = 0;
    bool operator==(const ActiveZone &other) const
    {
        return zone == other.zone && world == other.world && zone_name == other.zone_name && info == other.info &&
               entity_context == other.entity_context && script_context == other.script_context;
    }
};
ActiveZone read_active_zone(HANDLE process, std::uintptr_t base = 0);
bool death_link_level(const ActiveZone &zone);
struct SceneView
{
    std::string zone;
    std::uintptr_t zone_address = 0;
    std::array<float, 16> view_projection{};
    bool camera_active = false;
    std::array<float, 3> player_position{};
    bool player_active = false;
};
std::uintptr_t nst_module_base(HANDLE process);
std::vector<ZoneRecord> read_zone_registry(HANDLE process);
bool c1_entrance_progression_valid(const std::string &level, int index);
SceneView read_scene(HANDLE process);
struct EntranceAnchor
{
    std::string hub, level;
    std::array<float, 3> position;
};
// stationary entrance anchors from the hub catalogues
std::vector<EntranceAnchor> entrance_anchors();

struct C2PortalProbe
{
    bool hub_active = false;
    std::uintptr_t entity = 0;
    std::uintptr_t component = 0;
    std::uintptr_t data = 0;
    // Boss script bindings and current entry state.
    std::uintptr_t boss_component = 0;
    std::uintptr_t boss_data = 0;
    unsigned char entry_allowed = 0;
    std::uintptr_t entry_component = 0;
    std::uintptr_t entry_meta = 0;
    std::uintptr_t entry_gate = 0;
    std::array<std::uintptr_t, 2> effects{};
};

std::vector<std::string> c2_portal_levels();
struct C2BossEventProbe
{
    C2PortalProbe portal;
    std::uintptr_t entry_output = 0;
    std::uintptr_t original_output = 0;
    std::uintptr_t owner_accessor = 0;
    std::array<unsigned char, 512> templates{};
};
C2BossEventProbe probe_c2_boss_events_at(HANDLE process, std::uintptr_t entity, const std::string &level);
std::array<unsigned char, 512> c2_boss_event_copy(const C2BossEventProbe &probe, std::uintptr_t remote,
                                                  std::uintptr_t fallback = 0);
std::vector<unsigned char> c2_boss_event_chain(const std::vector<C2BossEventProbe> &probes, std::uintptr_t remote);
std::map<std::string, std::uintptr_t> find_c2_hub_entities(DWORD pid);

struct C2LiftProbe
{
    bool hub_active = false;
    std::uintptr_t entity = 0;
    std::uintptr_t component = 0;
    std::uintptr_t data = 0;
    std::array<std::uintptr_t, 2> permissions{};
    std::array<unsigned char, 2> values{};
    std::array<unsigned char, 2> input_values{};
};
C2LiftProbe probe_c2_lift_at(HANDLE process, std::uintptr_t entity, unsigned floor);
std::array<unsigned char, 232> c2_lift_data_override(std::array<unsigned char, 232> data, unsigned floor);
constexpr bool c2_lift_direction_allowed(unsigned floor, unsigned direction)
{
    return floor >= 1 && floor <= 5 && (direction == 0 ? floor < 5 : direction == 1 && floor > 1);
}

// re-check a previously located portal without repeating the full process scan
C2PortalProbe probe_c2_portal_at(HANDLE process, std::uintptr_t entity,
                                 const std::string &level, std::uintptr_t cloned_data = 0);

bool verify_c2_trigger_data(HANDLE process, std::uintptr_t data,
                            const std::string &level, bool events_enabled);

struct C3TriggerTypes
{
    std::uintptr_t component_meta = 0;
    std::uintptr_t data_meta = 0;
};
struct C3ButtonDiscovery
{
    std::map<std::string, std::uintptr_t> entities;
    C3TriggerTypes types;
};
C3ButtonDiscovery find_c3_buttons(DWORD pid);
std::vector<std::string> c3_button_keys();
C2PortalProbe probe_c3_button_at(HANDLE process, std::uintptr_t entity, const std::string &button,
                                 const C3TriggerTypes &types, std::uintptr_t cloned_data = 0);
bool verify_c3_trigger_data(HANDLE process, std::uintptr_t data, const C3TriggerTypes &types,
                            bool events_enabled);
std::vector<unsigned char> trigger_data_copy(std::vector<unsigned char> data);
// C3 activation scripts retain the game's busy/teleport checks.
struct C3ButtonEventProbe
{
    std::uintptr_t entity = 0, script = 0, meta = 0;
    std::uintptr_t entry_output = 0, original_output = 0, busy_check = 0, owner_accessor = 0;
    std::uintptr_t contact_output = 0, original_contact = 0;
    std::array<std::uintptr_t, 2> visibility_outputs{}, original_visibility{};
    std::array<unsigned char, 512> templates{};
};
struct C3BarrierEventProbe
{
    std::uintptr_t entity = 0, meta = 0, entry_output = 0, original_output = 0, disable = 0;
    std::uintptr_t false_value = 0, owner_accessor = 0;
    std::array<unsigned char, 48> enabled_property{};
};
C3ButtonEventProbe probe_c3_button_events_at(HANDLE process, std::uintptr_t entity, const std::string &key,
                                             std::uintptr_t override_route = 0, std::uintptr_t contact_override = 0);
std::array<C3BarrierEventProbe, 4> probe_c3_barriers_at(HANDLE process, std::uintptr_t button,
                                                        const std::string &key, std::uintptr_t override_route = 0);
std::vector<unsigned char> c3_button_event_chain(const std::vector<C3ButtonEventProbe> &probes,
                                                 std::uintptr_t remote);

std::vector<unsigned char> c3_barrier_event_chain(const C3ButtonEventProbe &button,
                                                 const std::array<C3BarrierEventProbe, 4> &barriers,
                                                 std::uintptr_t remote);

int c3_button_index(const std::string &name);

struct C3PortalDestination
{
    std::string key;
    std::uintptr_t zone = 0;
    std::array<unsigned char, 48> handle{};
};
struct C3PortalEntryProbe
{
    bool hub_active = false;
    std::uintptr_t meta = 0, entry_output = 0, original_output = 0, destination_accessor = 0;
    std::uintptr_t relic_output = 0, relic_original = 0, relic_unlocked = 0;
    std::array<std::uintptr_t, 7> owners{};
    std::array<unsigned char, 48> comparison{};
    std::array<unsigned char, 32> constant{};
    std::vector<C3PortalDestination> destinations;
};
C3PortalEntryProbe probe_c3_portal_entry_at(HANDLE process, std::uintptr_t button, const std::string &key,
                                            std::uintptr_t override_route = 0);
std::vector<unsigned char> c3_portal_entry_chain(const C3PortalEntryProbe &probe, std::uintptr_t remote);

struct C1MapDiscovery
{
    std::uintptr_t anchor = 0, manager_meta = 0;
};
struct C1MapProbe
{
    C1MapDiscovery discovery;
    C3PortalEntryProbe entry, secrets;
    std::uintptr_t progression_route = 0, original_progression = 0, bounds = 0;
    std::uintptr_t shortcut_route = 0, shortcut_denied = 0, shortcut_move = 0;
    std::uintptr_t false_value = 0, resume_input = 0;
    std::array<unsigned char, 40> loading_assignment{};
};
C1MapDiscovery find_c1_map(DWORD pid);
C1MapProbe probe_c1_map_at(HANDLE process, const C1MapDiscovery &found,
                           std::uintptr_t entry_override = 0, std::uintptr_t secret_override = 0,
                           bool traversal = false);
void verify_c1_map_restore(HANDLE process, const C1MapProbe &before);
std::vector<unsigned char> c1_map_entry_chain(const C1MapProbe &probe, std::uintptr_t remote);
std::vector<unsigned char> c1_map_secret_chain(const C1MapProbe &probe, std::uintptr_t remote);

// read-only discovery of saved gem variables and game route queries
using GemOwnership = std::array<std::array<bool, 6>, 3>; // Blue, Green, Orange, Purple, Red, Yellow.
struct GemDiscovery
{
    bool registry_metadata = false;
    std::uintptr_t power_meta = 0;
    std::uintptr_t ability_menu_meta = 0;
    std::uintptr_t crate_meta = 0;
    std::uintptr_t hud_meta = 0;
    // Scanning this process also finds temporary handle copies. Retain bytes, never their addresses.
    std::optional<std::array<unsigned char, 48>> secret_handle;
    std::array<std::optional<std::array<unsigned char, 48>>, 6> handles{};
    std::array<std::uintptr_t, 6> upgrades{}; // AP ability order, distinct from gem colors
    std::map<std::string, std::uintptr_t> graphs;
};

struct CrateProbe
{
    std::uintptr_t meta = 0, route = 0, original = 0, base = 0, identity = 0;
    unsigned add_offset = 0;
    std::array<unsigned char, 40> node{};
    std::array<unsigned char, 184> vtable{};
};
CrateProbe probe_crate_at(HANDLE process, std::uintptr_t meta, std::uintptr_t override_route = 0);
struct CrateMetadataPointer
{
    DWORD pid = 0;
    std::uintptr_t list = 0, slot = 0, meta = 0;
    std::array<unsigned char, 24> header{};
};
std::uintptr_t find_crate_metadata(DWORD pid, CrateMetadataPointer &cached);
std::vector<unsigned char> crate_break_stub(const CrateProbe &probe, std::uintptr_t queue);
struct GemTemplates
{
    std::uintptr_t comparison_vtable = 0, data_accessor_vtable = 0;
    std::array<unsigned char, 64> campaign{};
    std::array<unsigned char, 32> enumeration{}, constant{};
    std::array<unsigned char, 56> outputs{};
    std::array<std::array<unsigned char, 48>, 6> handles{};
};
struct GemRouteProbe
{
    std::string name;
    std::uintptr_t meta = 0, route = 0, original = 0, accessor = 0, yes = 0, no = 0;
    std::array<unsigned char, 40> test{};
    std::uintptr_t data_meta = 0, meta_name = 0, data_name = 0, dispatcher = 0;
    unsigned variable_offset = 0;
    unsigned component_size = 0, data_size = 0;
    std::uintptr_t variable_field = 0;
    std::uintptr_t effect_route = 0, effect_original = 0, effect_accessor = 0;
};
GemDiscovery find_gem_routes(DWORD pid, const GemDiscovery &cached = {});
GemDiscovery find_ability_metadata(DWORD pid);
GemTemplates probe_campaign_templates_at(HANDLE process, const GemDiscovery &found);
GemTemplates probe_gem_templates_at(HANDLE process, const GemDiscovery &found);
GemRouteProbe probe_gem_route_at(HANDLE process, const std::string &name,
                                 std::uintptr_t meta, std::uintptr_t override_route,
                                 std::uintptr_t data_accessor_vtable, std::uintptr_t effect_override = 0);
GemOwnership gem_ownership(const std::set<std::int64_t> &items, const std::set<char> &campaigns);
std::int64_t gem_item_id(unsigned campaign, unsigned color);
std::vector<unsigned char> gem_route_chain(const GemTemplates &templates, const GemRouteProbe &probe,
                                           std::uintptr_t remote, const GemOwnership &owned);

using AbilityOwnership = std::array<bool, 6>;
struct AbilityProbe
{
    std::uintptr_t meta = 0, jump_call = 0, jump_variable = 0, getter = 0;
    std::uintptr_t refresh_call = 0, execute = 0, actor_vtable = 0, component_vtable = 0;
    std::array<unsigned char, 24> method{};
    std::array<unsigned char, 56> nodes{}, entry{};
    std::array<std::uintptr_t, 5> inputs{}, original{};
    std::uintptr_t yes = 0, no = 0;
};
struct AbilityMenuProbe
{
    std::uintptr_t meta = 0, route = 0, original = 0, accessor = 0;
    std::array<unsigned char, 88> sender{};
    std::array<unsigned char, 56> arguments{};
    std::array<unsigned char, 32> yes{}, no{};
};
AbilityMenuProbe probe_ability_menu_at(HANDLE process, const GemDiscovery &found, std::uintptr_t override_route = 0);
std::vector<unsigned char> ability_menu_chain(const GemTemplates &templates, const AbilityMenuProbe &menu,
                                              std::uintptr_t remote, const AbilityOwnership &owned);
AbilityProbe probe_abilities_at(HANDLE process, const GemDiscovery &found, bool installed = false,
                                const std::array<unsigned char, 5> &jump_override = {},
                                const std::array<unsigned char, 5> &refresh_override = {});
AbilityOwnership ability_ownership(const std::set<std::int64_t> &items, const std::set<char> &campaigns);
std::array<unsigned char, 5> ability_jump_call(std::uintptr_t call, std::uintptr_t target);
std::vector<unsigned char> ability_jump_stub(const AbilityProbe &probe, std::uintptr_t remote, std::uintptr_t jump_filter = 0);
std::vector<unsigned char> ability_refresh_stub(const AbilityProbe &probe, std::uintptr_t remote, std::uintptr_t filler_tick = 0);
std::vector<unsigned char> ability_refresh_method(const AbilityProbe &probe, std::uintptr_t remote);

struct FillerProbe
{
    std::uintptr_t meta = 0, data_meta = 0, data_vtable = 0;
    std::uintptr_t world_slot = 0, camera_slot = 0, camera_vtable = 0;
    unsigned data_size = 0;
    std::array<std::array<unsigned char, 48>, 2> adds{}; // game pending life and Wumpa HUD counters
    std::array<std::array<unsigned char, 32>, 2> constants{};
    std::array<std::array<unsigned char, 88>, 2> show{};
};
FillerProbe probe_filler_at(HANDLE process, std::uintptr_t hud_meta);
unsigned wumpa_amount(unsigned receipt_index);
std::vector<unsigned char> filler_method(const AbilityProbe &method, const FillerProbe &filler, unsigned kind, std::uintptr_t remote, unsigned amount = 10);
std::vector<unsigned char> filler_stub(const AbilityProbe &actor, const FillerProbe &filler, std::uintptr_t remote);

struct GemRefreshProbe
{
    std::uintptr_t call = 0, context = 0, manager = 0, campaign = 0, list = 0, release = 0, execute = 0;
    std::uintptr_t component_vtable = 0, data_vtable = 0, meta_vtable = 0;
    std::uintptr_t events_meta = 0, events_vtable = 0;
    AbilityProbe method;
    std::array<std::uintptr_t, 6> variables{};
};
GemRefreshProbe probe_gem_refresh_at(HANDLE process, const GemDiscovery &found,
                                     const std::array<unsigned char, 5> &override_call = {});
std::vector<unsigned char> gem_refresh_stub(const GemRefreshProbe &probe,
                                            const std::vector<GemRouteProbe> &jobs, std::uintptr_t remote,
                                            std::uintptr_t addon_tick = 0);
std::uintptr_t probe_secret_variable(HANDLE process, const std::array<unsigned char, 48> &handle);

struct C3PlatformProbe
{
    std::array<std::uintptr_t, 3> entities{}; // outbound owner, return owner, floor cap
    std::array<unsigned char, 3> active{};
    std::uintptr_t false_value = 0;
    std::array<unsigned char, 48> enabled_property{};
};
C3PlatformProbe probe_c3_platform_at(HANDLE process, std::uintptr_t button, const std::string &key, bool placed = true);
std::vector<unsigned char> c3_platform_event_copy(const C3ButtonEventProbe &button,
                                                  const C3PlatformProbe &platform,
                                                  std::uintptr_t remote, std::uintptr_t continuation);
