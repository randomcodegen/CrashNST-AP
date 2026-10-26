#include "game_runtime.h"
#include "engine_memory.h"
#include "function_hook.h"
#include "movement_rules.h"
#include "game_pickups.h"
#include "game_memory.h"
#include "crate_specs.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <atomic>
#include <mutex>
#include <memory>
#include <iostream>
#include <set>
#include <string_view>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace ingame
{
    unsigned hub_campaign(std::string zone)
    {
        std::transform(zone.begin(), zone.end(), zone.begin(), [](unsigned char c)
                       { return std::tolower(c); });
        if (zone.rfind("maps/", 0) == 0)
            zone.erase(0, 5);
        for (unsigned i = 1; i <= 3; ++i)
        {
            const auto campaign = std::to_string(i), hub = "l" + campaign + "00_hub";
            if (zone == "crash" + campaign + "/" + hub + "/" + hub)
                return i;
        }
        return 0;
    }
    std::string level_key(std::string zone)
    {
        std::transform(zone.begin(), zone.end(), zone.begin(), [](unsigned char c)
                       { return std::tolower(c); });
        if (zone.rfind("maps/", 0) == 0)
            zone.erase(0, 5);
        const auto slash = zone.find('/');
        if (slash == std::string::npos || zone.size() < slash + 6)
            return {};
        auto key = zone.substr(slash + 1, 4);
        if ((key[0] != 'l' && key[0] != 'b') || key.substr(1, 3) == "100" || key.substr(1, 3) == "200" || key.substr(1, 3) == "300")
            return {};
        key[0] = static_cast<char>(std::toupper(key[0]));
        return key;
    }
    namespace { bool c3_platform_idle(const C3PlatformProbe &platform); }
    bool initialize_c3_platform(const C3ButtonEventProbe &button, const C3PlatformProbe &platform,
                                std::pair<std::uintptr_t, std::uintptr_t> &initialized)
    {
        using namespace engine_memory;
        if (!button.script || !platform.entities[2])
            throw std::runtime_error("C3 platform script context is unavailable");
        const auto identity = std::make_pair(platform.entities[2], button.script);
        const bool recover = !platform.active[0] && !platform.active[1] && !platform.active[2] &&
                             c3_platform_idle(platform);
        if (initialized == identity && !recover)
            return false;
        if (platform.active[2] || recover)
        {
            alignas(8) std::array<unsigned char, 72> method{};
            if (button.meta < 42340 || !read(button.meta - 42340 + 51828, method))
                throw std::runtime_error("C3 platform method context is unavailable");
            alignas(8) std::array<unsigned char, 3 * 256> nodes{};
            const auto actions = c3_platform_event_copy(button, platform, reinterpret_cast<Address>(nodes.data()), 0);
            std::copy(actions.begin(), actions.end(), nodes.begin());
            const auto address = reinterpret_cast<Address>(method.data());
            const auto pointer = [&](unsigned offset, Address value) { std::memcpy(method.data() + offset, &value, 8); };
            pointer(16, address + 24);
            pointer(56, address + 64);
            pointer(64, reinterpret_cast<Address>(nodes.data()));
            // the method executor owns a complete frame and runs these stack-backed actions before returning
            reinterpret_cast<void (*)(Address, Address, unsigned, Address)>(base() + 0xc988c0)(address, button.script, 0, 0);
        }
        // after bootstrap the path scripts own the lift, including closing the floor while it moves
        if (value<unsigned>(platform.entities[2] + 56) & 0x100)
            return false;
        initialized = identity;
        return platform.active[2] != 0 || recover;
    }
    namespace
    {
        using namespace engine_memory;
        FunctionHook hub_bool_hook, hub_int_hook, hub_gate_query_hook, c2_secret_entry_hook, c2_boss_portal_hook;
        std::atomic<unsigned> hub_areas{0x7fff};
        struct SecretReturn
        {
            Address script = 0, owner = 0, gate = 0, meta = 0;
        };
        std::array<SecretReturn, 5> secret_returns{};
        bool vehicle_death_level();
        void vehicle_death_event(Address node, Address context);

        Address c2_boss_portal_event(Address node, Address context)
        {
            vehicle_death_event(node, context);
            const auto instance = value<Address>(context), meta = value<Address>(instance + 56);
            if (meta >= 16488 && node == meta - 16488 + 24272 && typed(node, 0x13bc390) &&
                typed(instance, 0x13bbbd0) && typed(meta, 0x1745d78) &&
                value<unsigned>(meta + 24) == 35 && value<unsigned short>(meta + 90) == 156 &&
                string(value<Address>(meta + 16)) == "Scripts.Graph.common_C2_Hub_BossPortal")
            {
                const auto owner = value<Address>(instance + 24);
                const auto name = string(value<Address>(owner + 128));
                constexpr std::array<const char *, 5> bosses{{"B201_RipperRoo_BossLevelPortal",
                    "B202_KomodoBrothers_BossLevelPortal", "B203_TinyTiger_BossLevelPortal",
                    "B204_DrNGin_BossLevelPortal", "B205_DrNeoCortex_BossLevelPortal"}};
                // AP gates own level access. a pending vanilla boss event blocks the rest of the floor.
                if (typed(owner, 0x14c8ef8) && std::find(bosses.begin(), bosses.end(), name) != bosses.end())
                    return value<Address>(node + 72);
            }
            return reinterpret_cast<Address (*)(Address, Address)>(c2_boss_portal_hook.original)(node, context);
        }

        Address c2_secret_entry_gate(Address node, Address context)
        {
            const auto instance = value<Address>(context), meta = value<Address>(instance + 56);
            if (typed(instance, 0x13bbbd0) && typed(meta, 0x1745d78) &&
                string(value<Address>(meta + 16)) == "Scripts.Graph.common_C2_WarpRoom_LevelPortal" &&
                value<unsigned short>(meta + 90) == 836 && node == meta - 144092 + 155996 &&
                typed(node, 0x1759ef0) && typed(meta - 144092 + 138076, 0x17a1858) &&
                value<unsigned short>(meta - 144092 + 138076 + 24) == 392)
            {
                const auto owner = value<Address>(instance + 24);
                const auto name = string(value<Address>(owner + 128));
                constexpr std::array<const char *, 5> secrets{{"L202_SnowGo_Secret_Portal", "L207_AirCrash_Secret_Portal",
                    "L214_RoadToRuin_Secret_Portal", "L226_TotallyBear_Secret_Portal", "L227_TotallyFly_Secret_Portal"}};
                const auto secret = std::find(secrets.begin(), secrets.end(), name);
                if (typed(owner, 0x14c8ef8) && secret != secrets.end())
                {
                    const auto slab = meta - 144092, gate = value<Address>(instance + 104);
                    // reload can start a saved secret-return sequence with no previous level to finish it
                    if (!value<Address>(instance + 136) && value<unsigned char>(instance + 760) == 1 &&
                        typed(gate, 0x1759da0) && value<int>(gate + 16) == 0 && !value<unsigned char>(gate + 20) &&
                        value<unsigned short>(slab + 146332 + 24) == 136 &&
                        typed(slab + 137228, 0x17a1858) && value<unsigned short>(slab + 137228 + 24) == 760 &&
                        typed(slab + 159268, 0x1759fa8) && value<Address>(slab + 159268 + 16) == slab + 156020)
                    {
                        secret_returns[secret - secrets.begin()] = {instance, owner, gate, meta};
                        return 0;
                    }
                    // contact during hub initialization must not consume the gate before entry is ready
                    if (!value<unsigned char>(instance + 392))
                        return 0;
                }
            }
            return reinterpret_cast<Address (*)(Address, Address)>(c2_secret_entry_hook.original)(node, context);
        }

        void hub_gate_query(Address context, Address args, Address outputs)
        {
            const auto instance = value<Address>(context);
            const auto meta = value<Address>(instance + 56);
            const auto inputs = value<Address>(args + 16);
            if (typed(instance, 0x13bbbd0) && typed(meta, 0x1745d78) &&
                string(value<Address>(meta + 16)) == "Scripts.Graph.L300_Hub_C3_Hub_ElectricGate" &&
                value<Address>(inputs) == meta - 23764 + 20316 &&
                value<Address>(inputs + 8) == meta - 23764 + 20508 &&
                typed(meta - 23764 + 20316, 0x1747910) && typed(meta - 23764 + 20508, 0x1749820) &&
                value<unsigned>(meta - 23764 + 20508 + 24) == 0x800)
            {
                const auto branches = list(outputs, 2);
                const auto owner = value<Address>(instance + 24);
                const auto name = string(value<Address>(owner + 128));
                const bool known = typed(owner, 0x141d490) && name.size() == 21 &&
                                   name.compare(0, 20, "C3_Hub_ElectricGate0") == 0 && name[20] >= '2' && name[20] <= '5';
                const bool unlocked = known && (hub_areas.load(std::memory_order_relaxed) & (1u << (10 + name[20] - '1')));
                // OnStart otherwise disables gates from saved boss rewards before the contact adapter attaches
                if (branches.size() == 2)
                    reinterpret_cast<void (*)(Address, Address)>(base() + 0xcb6db0)(context, branches[unlocked ? 0 : 1]);
                return;
            }
            reinterpret_cast<void (*)(Address, Address, Address)>(hub_gate_query_hook.original)(context, args, outputs);
        }

        unsigned hub_islands()
        {
            const auto mask = hub_areas.load(std::memory_order_relaxed);
            return (mask & 6) == 6 ? 3 : (mask & 2) ? 2 : 1;
        }

        bool hub_bool_field(Address accessor, Address context)
        {
            // getter arguments and their field metadata are owned by the engine for this call
            const auto field = *reinterpret_cast<const Address *>(accessor + 24);
            const auto instance = *reinterpret_cast<const Address *>(context);
            if (field && instance && *reinterpret_cast<const Address *>(instance) == base() + 0x13bbbd0)
            {
                const auto meta = *reinterpret_cast<const Address *>(instance + 56);
                const auto name = meta ? *reinterpret_cast<const char *const *>(meta + 16) : nullptr;
                // compiled C1 accessor descriptors omit names. match the verified descriptor and instance offset.
                if (name && !std::strcmp(name, "Scripts.Graph.common_Crash1_Hub_Manager") &&
                    field == meta - 142276 + 117340 && *reinterpret_cast<const unsigned short *>(field + 24) == 64)
                    return false;
                const auto offset = *reinterpret_cast<const unsigned short *>(field + 24);
                // +224/+225 cache HUD choices and must still clear when the player leaves the lift
                if (name && (offset == 161 || offset == 162) &&
                    !std::strcmp(name, "Scripts.Graph.common_Crash2_Hub_WarpLift"))
                {
                    const auto entity = *reinterpret_cast<const Address *>(instance + 24);
                    const auto owner = entity ? *reinterpret_cast<const char *const *>(entity + 128) : nullptr;
                    if (owner && !std::strncmp(owner, "Floor0", 6) && owner[6] >= '1' && owner[6] <= '5' &&
                        !std::strcmp(owner + 7, "_WarpLift"))
                    {
                        const auto floor = unsigned(owner[6] - '0');
                        // floor 1 descends to the secret room using its normal discovery flag.
                        if (floor == 1 && offset == 162)
                            return value<unsigned char>(instance + offset) != 0;
                        const auto area = offset == 161 ? floor + 1 : floor - 1;
                        return area >= 1 && area <= 5 && (hub_areas.load(std::memory_order_relaxed) & (1u << (area + 4)));
                    }
                }
            }
            return reinterpret_cast<bool (*)(Address, Address)>(hub_bool_hook.original)(accessor, context);
        }

        int hub_int_field(Address accessor, Address context)
        {
            const auto original = reinterpret_cast<int (*)(Address, Address)>(hub_int_hook.original)(accessor, context);
            const auto field = *reinterpret_cast<const Address *>(accessor + 24);
            const auto instance = *reinterpret_cast<const Address *>(context);
            if (field && instance && *reinterpret_cast<const Address *>(instance) == base() + 0x13bbbd0)
            {
                const auto meta = *reinterpret_cast<const Address *>(instance + 56);
                const auto name = meta ? *reinterpret_cast<const char *const *>(meta + 16) : nullptr;
                const auto offset = *reinterpret_cast<const unsigned short *>(field + 24);
                // without a physical return, the saved vanilla progression must use the graph's default spawn (1)
                if (name && !std::strcmp(name, "Scripts.Graph.common_Crash1_Hub_Manager") &&
                    field == meta - 142276 + 113076 &&
                    !value<Address>(instance + value<unsigned short>(meta - 142276 + 147404 + 24)))
                    return 1;
                // Only this navigation comparison uses the count. The separate initialization count must stay complete.
                if (name && !std::strcmp(name, "Scripts.Graph.common_Crash1_Hub_Manager") &&
                    field == meta - 142276 + 120244 && offset == 152)
                    return std::min(original, static_cast<int>(hub_islands()));
                // Reject a direction in the input router, before it changes map indices or starts the camera.
                if (name && !std::strcmp(name, "Scripts.Graph.common_Crash1_Hub_Manager") &&
                    field == meta - 142276 + 145340 && offset == 176)
                {
                    const auto island = *reinterpret_cast<const int *>(instance + 192);
                    const auto spot = *reinterpret_cast<const int *>(instance + 208);
                    const auto end = *reinterpret_cast<const int *>(instance + 204);
                    const auto islands = static_cast<int>(hub_islands());
                    const auto stormy = (original == 5 || original == 6) &&
                        string(value<Address>(value<Address>(instance + 72) + 128)) == "Isle01_MapSpot_00_StormyTeleport";
                    // direction 4 takes the engine's no-move/input-resume branch. counts used by initialization stay intact.
                    if (island < 0 || island > 2 ||
                        (original == 1 && (spot < 0 || end <= 0 || end > 64 || (spot >= end - 1 && island + 1 >= islands))) ||
                        (island == 0 && islands < 3 && ((original == 0 && spot <= 0) || stormy)))
                        return 4;
                }
            }
            return original;
        }
        bool event_owner(Address entity)
        {
            return entity && (typed(entity, 0x1592220) || typed(entity, 0x14c8ef8) || typed(entity, 0x14c7fb0) || typed(entity, 0x141d490)) && (value<unsigned>(entity + 56) & 4);
        }
        std::vector<Address> event_owners()
        {
            // context+120 maps metadata keys (+40) to parallel instance lists (+24). select CEntityEvents.
            const auto context = value<Address>(base() + 0x1ac1dd8), meta = value<Address>(base() + 0x1a744a0);
            const auto table = value<Address>(context + 120);
            const auto bytes = value<std::uint64_t>(table + 16) & 0xfffffffffffffULL;
            if (!context || !meta || !table || bytes != (value<std::uint64_t>(table + 32) & 0xfffffffffffffULL) || bytes > 65536 * 8 || bytes % 8)
                return {};
            const auto keys = value<Address>(table + 40), lists = value<Address>(table + 24);
            Address events = 0;
            for (unsigned i = 0; i < bytes / 8; ++i)
                if (value<Address>(keys + i * 8) == meta)
                {
                    events = value<Address>(lists + i * 8);
                    break;
                }
            std::vector<Address> result;
            for (const auto event : list(events, 16384))
                if (typed(event, 0x14cb600))
                {
                    const auto actor = value<Address>(event + 24);
                    if (event_owner(actor))
                        result.push_back(actor);
                }
            return result;
        }

        FunctionHook ui_audio_hook;
        std::atomic<Address> permanent_audio_pool{0};
        bool shared_ui_audio(Address path)
        {
            std::array<char, 256> bytes{};
            if (!read(path, bytes))
                return false;
            const auto end = static_cast<char *>(std::memchr(bytes.data(), 0, bytes.size()));
            if (!end)
                return false;
            std::transform(bytes.data(), end, bytes.data(), [](unsigned char c) { return std::tolower(c); });
            std::string_view name(bytes.data(), end - bytes.data());
            // loaders can receive archive paths before the engine expands their virtual-directory aliases
            if (name.rfind("sounds:", 0) == 0)
                name.remove_prefix(7);
            else if (name.rfind("sounds/", 0) == 0)
                name.remove_prefix(6);
            for (const auto bank : {"/banks/ui_bank_c1_bank.igz", "/banks/ui_bank_c2_bank.igz", "/banks/ui_bank_c3_bank.igz"})
                if (name == bank)
                    return true;
            if (name.rfind("data:/", 0) == 0)
                name.remove_prefix(6);
            if (name.rfind("sound_samples/", 0) == 0)
                for (const auto bank : {",ui_bank_c1_bank,", ",ui_bank_c2_bank,", ",ui_bank_c3_bank,"})
                    if (name.find(bank) != name.npos)
                        return true;
            return false;
        }
        Address load_ui_audio(Address path, Address pool, unsigned flags)
        {
            if (shared_ui_audio(path))
            {
                auto permanent = permanent_audio_pool.load();
                if (!permanent)
                {
                    // the streaming manager's inline chunk vector has count +24, byte capacity +32 and entries +40
                    const auto manager = value<Address>(base() + 0x1aa9eb8);
                    const auto count = value<unsigned>(manager + 24);
                    const auto capacity = value<std::uint64_t>(manager + 32) & 0xfffffffffffffULL;
                    if (typed(manager, 0x1616f60) && count <= 4096 && capacity >= count * 8)
                        for (unsigned i = 0; i < count; ++i)
                        {
                            const auto chunk = value<Address>(value<Address>(manager + 40) + i * 8);
                            if (typed(chunk, 0x1615820) && string(value<Address>(chunk + 16)) == "permanent_pkg")
                            {
                                permanent = value<Address>(chunk + 96);
                                if (!typed(permanent, 0x17a2808))
                                    permanent = 0;
                                permanent_audio_pool = permanent;
                                break;
                            }
                        }
                }
                // C2's hub shares C3 UI audio. a level pool reset would invalidate the persistent campaign package.
                if (permanent)
                    pool = permanent;
            }
            return reinterpret_cast<Address (*)(Address, Address, unsigned)>(ui_audio_hook.original)(path, pool, flags);
        }
        FunctionHook license_hook;
        void license_query(unsigned key, unsigned char *result)
        {
            // option 460 is license acceptance. inheritance changes only this key in the verified game-state object.
            reinterpret_cast<void (*)(unsigned, unsigned char *)>(license_hook.original)(key, result);
            if (key != 460 || !result || *result)
                return;
            const auto state = value<Address>(value<Address>(base() + 0x1a69a98) + 32);
            if (typed(state, 0x155e730))
            {
                reinterpret_cast<void (*)(Address, unsigned, unsigned char)>(base() + 0x4fdb50)(state, 460, 1);
                *result = 1;
            }
        }
        bool special_level(const std::string &key)
        {
            static const std::vector<std::string> levels{"L107", "L114", "L208", "L213", "L215", "L222", "L224", "L226",
                                                         "L302", "L303", "L305", "L308", "L310", "L312", "L314", "L317", "L318", "L322", "L324", "L326", "L328", "L330", "L331", "L332"};
            return std::find(levels.begin(), levels.end(), key) != levels.end();
        }
        std::mutex movement_mutex;
        std::map<std::string, unsigned> movement_styles;
        std::atomic_bool vanilla_movement{false};
        FunctionHook movement_check_hook, movement_input_hook, movement_attack_hook, movement_skill_hook;
        Address missing_crouch_actor = 0, movement_input_code = 0;
        std::atomic<Address> blocked_crouch_controller{0};
        unsigned assigned_movement()
        {
            const auto world = value<Address>(base() + 0x1a64690);
            const auto key = level_key(string(value<Address>(world + 184)));
            if (special_level(key))
                return 0;
            std::lock_guard lock(movement_mutex);
            const auto found = movement_styles.find(key);
            return found == movement_styles.end() ? 0 : found->second;
        }
        unsigned scene_campaign()
        {
            const auto world = value<Address>(base() + 0x1a64690);
            return world ? reinterpret_cast<unsigned (*)(Address)>(base() + 0x131400)(world) : 0;
        }
        unsigned effective_movement()
        {
            return movement_style(assigned_movement(), scene_campaign(), vanilla_movement);
        }
        std::uint64_t movement_attack(Address component, Address input, unsigned char ignore_blocks)
        {
            const auto selected = reinterpret_cast<std::uint64_t (*)(Address, Address, unsigned char)>(movement_attack_hook.original)(component, input, ignore_blocks);
            if (selected < 4)
            {
                const auto style = effective_movement();
                const auto actor = value<Address>(component + 24);
                if (style == 1 && typed(component, 0x1427ae0) && actor && actor == player() && typed(value<Address>(actor + 424), 0x1589128))
                {
                    // filter pressed and buffered slides before the attack state or animation changes
                    const auto event = reinterpret_cast<Address (*)(Address, unsigned)>(base() + 0x47cfa0)(component, static_cast<unsigned>(selected));
                    return movement_attack_selection(static_cast<unsigned>(selected), style, string(event));
                }
            }
            return selected;
        }
        unsigned char movement_skill(Address filter, Address actor)
        {
            const auto original = reinterpret_cast<unsigned char (*)(Address, Address)>(movement_skill_hook.original)(filter, actor);
            if (typed(filter, 0x1496238) && actor && actor == player() && typed(value<Address>(actor + 424), 0x1589128))
            {
                // OButton is the base slide/body-slam skill. AP power skills retain their original ownership checks.
                const auto skill = resolve(value<Address>(filter + 24));
                if (typed(skill, 0x15ad140))
                    return movement_skill_filter(original != 0, effective_movement(), value<unsigned>(filter + 16),
                                                string(value<Address>(skill + 24)));
            }
            return original;
        }
        void movement_check(Address context, Address args, Address outputs)
        {
            const auto accessor = value<Address>(value<Address>(args + 16));
            bool crouch = false, jump = false;
            if (typed(accessor, 0x1749820) && value<unsigned>(accessor + 24) == 1)
                for (const auto &[delta, name] : std::array<std::pair<unsigned, const char *>, 3>{{
                         {824, "Scripts.Graph.Crash_CrouchCrawlSlide"},
                         {3856, "Scripts.Graph.CrashBandicoot_JumpFlip"},
                         {3104, "Scripts.Graph.CrashBandicoot_JumpFlip"}}})
                {
                    // Slab distances identify these three graph constants without changing shared script data.
                    const auto meta = accessor + delta;
                    if (typed(meta, 0x1745d78) && string(value<Address>(meta + 16)) == name)
                    {
                        crouch = delta == 824;
                        jump = !crouch;
                        break;
                    }
                }
            const auto branches = (crouch || jump) ? list(outputs, 2) : std::vector<Address>{};
            if (branches.size() == 2)
            {
                // OnStart removes crouch in Crash 1. retain it before initialization for either advanced style
                // retaining it across F10 switches is safe because the input hook blocks Crash 1 crouch
                const bool matches = crouch ? !movement_needs_crouch(assigned_movement(), scene_campaign())
                                            : effective_movement() == 1;
                reinterpret_cast<void (*)(Address, Address)>(base() + 0xcb6db0)(context, branches[matches ? 0 : 1]);
                return;
            }
            reinterpret_cast<void (*)(Address, Address, Address)>(movement_check_hook.original)(context, args, outputs);
        }
        Address last_actor = 0;
        bool last_vehicle_death = false;
        std::atomic_bool remote_death{false};
        std::atomic<Address> death_handle{0};
        std::atomic_uint observed_deaths{0};
        FunctionHook death_hook;
        std::uint64_t remote_deadline = 0;
        bool bonus_round_active();
        Address bound_player_died()
        {
            const auto handle = death_handle.load(), variable = resolve(handle);
            if ((value<unsigned>(handle) & 0xf0000000) != 0x40000000 ||
                string(value<Address>(handle + 8)) != "PlayerDied" || string(value<Address>(handle + 24)) != "gamevariables" ||
                !typed(variable, 0x155ebf0) || value<unsigned>(variable + 16) > 2)
                return 0;
            return variable;
        }
        __declspec(noinline) void record_death_change(bool before, bool after, bool bonus)
        {
            if (before == after)
                return;
            if (!after)
            {
                remote_death = false;
                return;
            }
            if (!remote_death.exchange(false) && !bonus)
                ++observed_deaths;
        }
        void player_died_set(Address variable, unsigned char requested)
        {
            const bool tracked = variable && variable == bound_player_died() && !vehicle_death_level() &&
                                 !level_key(string(value<Address>(value<Address>(base() + 0x1a64690) + 184))).empty();
            const auto getter = reinterpret_cast<unsigned char (*)(Address)>(base() + 0x4e5c60);
            const bool before = tracked && getter(variable);
            // respawn callbacks can clear the bonus flag during the original setter
            const bool bonus = tracked && !before && requested && bonus_round_active();
            reinterpret_cast<void (*)(Address, unsigned char)>(death_hook.original)(variable, requested);
            if (tracked && variable == bound_player_died())
                record_death_change(before, getter(variable) != 0, bonus);
        }
        bool death_controls_ready(Address actor, bool camera_active)
        {
            // require an active, controllable player. skip transition/control-lock and invulnerability states.
            if (!camera_active || !typed(actor, 0x1592220) || !(value<unsigned>(actor + 56) & 0x100) ||
                value<unsigned char>(actor + 341) != 1 || value<unsigned char>(actor + 342) ||
                (value<unsigned>(actor + 336) & 0x30) || !value<unsigned char>(actor + 568))
                return false;
            const auto input = value<Address>(actor + 424), invulnerable = value<Address>(actor + 264);
            if (!typed(input, 0x1589128) || !typed(invulnerable, 0x13eb4b8))
                return false;
            const auto controls = value<Address>(input + 88);
            return typed(controls, 0x13eb4b8) && value<int>(controls + 16) == 0 && value<int>(invulnerable + 16) == 0;
        }
        __declspec(noinline) void update_c2_secret_returns(unsigned campaign)
        {
            if (campaign != 2)
            {
                secret_returns = {};
                return;
            }
            const auto actor = player();
            if (!death_controls_ready(actor, true))
                return;
            std::array<float, 3> position{};
            if (!read(actor + 32, position) ||
                !std::all_of(position.begin(), position.end(), [](float f) { return std::isfinite(f); }))
                return;
            for (auto &pending : secret_returns)
            {
                if (!pending.script)
                    continue;
                const auto [script, owner, gate, meta] = pending;
                const auto slab = meta - 144092;
                if (!typed(script, 0x13bbbd0) || !typed(owner, 0x14c8ef8) || !typed(gate, 0x1759da0) ||
                    !typed(meta, 0x1745d78) || value<Address>(script + 24) != owner ||
                    value<Address>(script + 56) != meta || value<Address>(script + 104) != gate ||
                    value<Address>(script + 136) || value<int>(gate + 16) || value<unsigned char>(gate + 20) ||
                    !value<unsigned char>(script + 760) || !typed(slab + 159268, 0x1759fa8) ||
                    value<Address>(slab + 159268 + 16) != slab + 156020)
                {
                    pending = {};
                    continue;
                }
                std::array<float, 3> center{};
                if (!value<unsigned char>(script + 392) || !read(owner + 32, center) ||
                    !std::all_of(center.begin(), center.end(), [](float f) { return std::isfinite(f); }))
                    continue;
                // All five secret triggers have radius 150. wait outside twice that radius with free controls.
                if (std::hypot(position[0] - center[0], position[1] - center[1]) <= 300)
                    continue;
                pending = {};
                Address context = script;
                reinterpret_cast<Address (*)(Address, Address)>(base() + 0xcb3a60)(slab + 159268,
                                                                                 reinterpret_cast<Address>(&context));
            }
        }
        std::atomic_uint active_style{0};
        using Matrix = std::array<float, 16>;
        Matrix multiply(const Matrix &a, const Matrix &b)
        {
            Matrix out{};
            for (unsigned r = 0; r < 4; ++r)
                for (unsigned c = 0; c < 4; ++c)
                    for (unsigned k = 0; k < 4; ++k)
                        out[r * 4 + c] += a[r * 4 + k] * b[k * 4 + c];
            return out;
        }
        bool entity_matrix(Address entity, Matrix &out, std::vector<Address> chain = {})
        {
            if (!entity || chain.size() >= 8 || std::find(chain.begin(), chain.end(), entity) != chain.end())
                return false;
            chain.push_back(entity);
            // placeable: position +32, transform pointer +48, optional parent attachment +16
            const auto transform = value<Address>(entity + 48);
            if (transform)
            {
                if (!read(transform + 32, out))
                    return false;
            }
            else
            {
                out = {};
                out[0] = out[5] = out[10] = out[15] = 1;
                std::array<float, 3> pos{};
                if (!read(entity + 32, pos))
                    return false;
                std::copy(pos.begin(), pos.end(), out.begin() + 12);
            }
            if (!std::all_of(out.begin(), out.end(), [](float f)
                             { return std::isfinite(f); }) ||
                std::abs(out[15] - 1) > 0.001f)
                return false;
            const auto bolt = value<Address>(entity + 16);
            if (bolt)
            {
                const auto parent = resolve(value<Address>(bolt + 16));
                Matrix parent_matrix{};
                if (!entity_matrix(parent, parent_matrix, std::move(chain)))
                    return false;
                out = multiply(out, parent_matrix);
            }
            return true;
        }
        bool visible_placeable(Address at, Address project, Address root)
        {
            for (unsigned depth = 0; at && depth < 8; ++depth)
            {
                unsigned flags = 0;
                float alpha = 0;
                if (!typed(at, 0x161cf98) || value<Address>(at + 40) != project || !read(at + 16, flags) ||
                    (flags & 0x4900) || !read(at + 208, alpha) || !std::isfinite(alpha) || alpha < 0.99f || alpha > 1.01f)
                    return false;
                if (at == root)
                    return true;
                at = value<Address>(at + 72);
            }
            return false;
        }
        bool campaign_menu_visible_at(Address manager)
        {
            // the engine GUI uses igGuiComposition
            if (!typed(manager, 0x1556d90))
                return false;
            for (unsigned c = 0; c < 5; ++c)
            {
                const auto context = value<Address>(manager + 48 + c * 8);
                if (!typed(context, 0x161bb18) || value<unsigned char>(context + 209))
                    continue;
                for (auto project : list(value<Address>(context + 72), 64))
                {
                    if (!typed(project, 0x161d3e8))
                        continue;
                    const auto flags = value<unsigned char>(project + 52);
                    auto root = value<Address>(project + 16);
                    if (!(flags & 0x20) || (flags & 0x10) || value<Address>(project + 144) != context || !typed(root, 0x161cf98))
                        continue;
                    const auto asset = resolve(value<Address>(root + 64));
                    if (typed(asset, 0x161be40))
                    {
                        const auto composed = value<Address>(asset + 32);
                        if (composed == root || value<Address>(composed + 72) != root || value<Address>(root + 40) != project ||
                            (value<unsigned>(root + 16) & 0x4800))
                            continue;
                        root = composed;
                    }
                    if (!visible_placeable(root, project, root))
                        continue;
                    unsigned panels = 0;
                    for (const auto child : list(value<Address>(root + 80), 64))
                    {
                        if (value<Address>(child + 72) != root || !visible_placeable(child, project, root))
                            continue;
                        const auto name = string(value<Address>(child + 48));
                        panels |= name == "[Game_Select_C1]" ? 1 : name == "[Game_Select_C2]" ? 2
                                                               : name == "[Game_Select_C3]"   ? 4
                                                                                              : 0;
                    }
                    if (panels == 7)
                        return true;
                }
            }
            return false;
        }
    }
    bool campaign_menu_visible() { return campaign_menu_visible_at(value<Address>(base() + 0x1a8feb8)); }
    SceneView scene()
    {
        // resolve the live camera through its system and metadata. validate the matrix field before reading +512.
        SceneView result;
        const auto world = value<Address>(base() + 0x1a64690);
        result.zone = string(value<Address>(world + 184));
        auto actor = player();
        static Address map_actor = 0;
        static std::uint64_t next_map_actor = 0;
        if (hub_campaign(result.zone) == 1)
        {
            // C1 moves a separate overworld character. Hero_Crash stays near the map origin.
            if (GetTickCount64() >= next_map_actor)
            {
                map_actor = 0;
                next_map_actor = GetTickCount64() + 250;
                for (const auto entity : event_owners())
                    if (typed(entity, 0x14c8ef8) && string(value<Address>(entity + 128)) == "CrashHub" &&
                        (value<unsigned>(entity + 56) & 0x100))
                    {
                        map_actor = entity;
                        break;
                    }
            }
            actor = event_owner(map_actor) && typed(map_actor, 0x14c8ef8) &&
                            string(value<Address>(map_actor + 128)) == "CrashHub" &&
                            (value<unsigned>(map_actor + 56) & 0x100)
                        ? map_actor : 0;
        }
        else
        {
            map_actor = 0;
            next_map_actor = 0;
        }
        static Address coco_actor = 0;
        static unsigned coco_campaign = 0;
        static std::uint64_t next_coco_actor = 0;
        const auto campaign = hub_campaign(result.zone);
        if (!actor && (campaign == 2 || campaign == 3))
        {
            // switching to hub Coco transfers ownership but leaves the player's Hero handle pointing at Crash
            const auto players = list(value<Address>(value<Address>(base() + 0x1a665e8) + 16), 12);
            const auto controlled_coco = [&](Address entity)
            {
                const auto owner = resolve(value<Address>(entity + 328));
                return event_owner(entity) && typed(entity, 0x1592220) &&
                       (value<unsigned>(entity + 56) & 0x100) && value<unsigned char>(entity + 341) == 1 &&
                       string(value<Address>(entity + 128)) == "Coco_Character" && owner &&
                       std::find(players.begin(), players.end(), owner) != players.end();
            };
            if (coco_campaign != campaign || GetTickCount64() >= next_coco_actor)
            {
                coco_campaign = campaign;
                coco_actor = 0;
                next_coco_actor = GetTickCount64() + 250;
                for (const auto entity : event_owners())
                    if (controlled_coco(entity))
                    {
                        if (coco_actor && coco_actor != entity) { coco_actor = 0; break; }
                        coco_actor = entity;
                    }
            }
            actor = controlled_coco(coco_actor) ? coco_actor : 0;
        }
        else
        {
            coco_actor = 0;
            coco_campaign = 0;
            next_coco_actor = 0;
        }
        if (actor)
            result.player_active = read(actor + 32, result.player_position) &&
                                   std::all_of(result.player_position.begin(), result.player_position.end(),
                                               [](float f) { return std::isfinite(f); });
        const auto system = value<Address>(base() + 0x1a890e0), meta = value<Address>(base() + 0x1a891d0);
        if (!system || !meta)
            return result;
        static Address verified_meta = 0;
        if (meta != verified_meta)
        {
            if (string(value<Address>(meta + 16)) != "CCameraSystemInstance" || value<std::uint16_t>(meta + 90) != 976)
                return result;
            const auto count = value<unsigned>(meta + 24);
            const auto fields = value<Address>(meta + 40);
            bool found = false;
            if (count > 128 || !fields)
                return result;
            for (unsigned i = 0; i < count; ++i)
            {
                const auto field = value<Address>(fields + i * 8);
                if (string(value<Address>(field + 48)) == "_viewProjectionMatrix")
                    found = value<std::uint16_t>(field + 22) == 64 && value<std::uint16_t>(field + 24) == 512;
            }
            if (!found)
                return result;
            verified_meta = meta;
        }
        const auto cameras = list(value<Address>(system + 32), 4);
        if (cameras.empty())
            return result;
        const auto camera = cameras.front();
        if (value<Address>(camera) != value<Address>(meta + 56) || !value<unsigned char>(camera + 376) || !value<unsigned char>(camera + 377))
            return result;
        if (read(camera + 512, result.view_projection) && std::all_of(result.view_projection.begin(), result.view_projection.end(), [](float f)
                                                                      { return std::isfinite(f); }))
            result.camera_active = true;
        return result;
    }
    namespace
    {
        struct Crate
        {
            Address anchor = 0, entity = 0, model = 0;
            Box identity;
        };
        struct TintedModel
        {
            Address model = 0, bundle = 0;
            unsigned color = 0;
            Address handle = 0;
            float opacity = default_tint_opacity;
        };
        std::mutex crate_mutex;
        std::vector<TintedModel> tinted_models;

        bool field_at(Address meta, const char *name, unsigned offset, unsigned size)
        {
            const auto count = value<unsigned>(meta + 24);
            const auto fields = value<Address>(meta + 40);
            if (!fields || count > 256)
                return false;
            for (unsigned i = 0; i < count; ++i)
            {
                const auto field = value<Address>(fields + i * 8);
                if (string(value<Address>(field + 48)) == name)
                    return value<std::uint16_t>(field + 24) == offset && value<std::uint16_t>(field + 22) == size;
            }
            return false;
        }
        __declspec(noinline) bool bonus_round_active()
        {
            // standard bonus rounds belong to a level manager, not a component on Crash
            for (const auto owner : event_owners())
                for (const auto component : list(value<Address>(owner + 24), 128))
                {
                    const auto meta = value<Address>(component + 56), data = value<Address>(component + 16);
                    if (!typed(component, 0x13bbbd0) || value<Address>(component + 24) != owner ||
                        !(value<unsigned>(component + 32) & 1) || (value<unsigned>(component + 32) & 24) ||
                        !(value<unsigned char>(component + 40) & 1) || !typed(meta, 0x1745d78) ||
                        string(value<Address>(meta + 16)) != "Scripts.Graph.common_Basic_BonusRound_Manager" ||
                        !typed(data, 0x13bbb10))
                        continue;
                    const auto dm = value<Address>(data + 32);
                    if (!dm || dm != value<Address>(meta + 176))
                        continue;
                    const auto count = value<unsigned>(dm + 24);
                    const auto size = value<std::uint16_t>(dm + 90);
                    const auto fields = value<Address>(dm + 40);
                    if (!fields || count > 256 || size < 40 || size > 4096)
                        continue;
                    for (unsigned i = 0; i < count; ++i)
                    {
                        const auto field = value<Address>(fields + i * 8);
                        if (!typed(field, 0x17a46b8) || string(value<Address>(field + 48)) != "BonusRoundActive_id_esgpg6p3" ||
                            value<std::uint16_t>(field + 22) != 8)
                            continue;
                        const auto offset = value<std::uint16_t>(field + 24);
                        if (offset < 40 || offset + 8u > size)
                            return false;
                        const auto handle = value<Address>(data + offset), variable = resolve(handle);
                        if ((value<unsigned>(handle) & 0xf0000000) != 0x40000000 ||
                            string(value<Address>(handle + 8)) != "BonusRoundActive" ||
                            string(value<Address>(handle + 24)) != "gamevariables" ||
                            !typed(variable, 0x155ebf0) || value<unsigned>(variable + 16) > 2 ||
                            !code_matches(0x4e5c60, std::array<unsigned char, 5>{0xe9, 0xab, 0xe9, 0xff, 0xff}))
                            return false;
                        return reinterpret_cast<unsigned char (*)(Address)>(base() + 0x4e5c60)(variable) != 0;
                    }
                }
            return false;
        }
        bool vehicle_death_level()
        {
            const auto key = level_key(string(value<Address>(value<Address>(base() + 0x1a64690) + 184)));
            return key == "L305" || key == "L318" || key == "L326" || key == "L331" ||
                   key == "L317" || key == "L324" || key == "L330";
        }
        __declspec(noinline) Address vehicle_death_component()
        {
            Address found = 0;
            for (const auto owner : event_owners())
                for (const auto component : list(value<Address>(owner + 24), 128))
                {
                    const auto meta = value<Address>(component + 56), data = value<Address>(component + 16);
                    if (!typed(component, 0x13bbbd0) || value<Address>(component + 24) != owner ||
                        !(value<unsigned>(component + 32) & 1) || (value<unsigned>(component + 32) & 24) ||
                        !(value<unsigned char>(component + 40) & 1) || !typed(meta, 0x1745d78) ||
                        string(value<Address>(meta + 16)) != "Scripts.Graph.Crash_Vehicle_Damage_Manager" ||
                        value<unsigned>(meta + 24) != 51 || !typed(data, 0x13bbb10))
                        continue;
                    if (found && found != component)
                        return 0;
                    found = component;
                }
            return found;
        }
        __declspec(noinline) Address vehicle_death_handle(Address component)
        {
            const auto meta = value<Address>(component + 56), data = value<Address>(component + 16);
            const auto dm = value<Address>(data + 32);
            if (!typed(component, 0x13bbbd0) || !typed(meta, 0x1745d78) || !typed(data, 0x13bbb10) ||
                !(value<unsigned>(component + 32) & 1) || (value<unsigned>(component + 32) & 24) ||
                !(value<unsigned char>(component + 40) & 1) ||
                string(value<Address>(meta + 16)) != "Scripts.Graph.Crash_Vehicle_Damage_Manager" ||
                value<unsigned>(meta + 24) != 51 || !typed(dm, 0x1745cc0) || dm != value<Address>(meta + 176) ||
                string(value<Address>(dm + 16)) != "Scripts.Graph.Crash_Vehicle_Damage_ManagerData" ||
                value<unsigned>(dm + 24) != 23)
                return 0;
            const auto size = value<std::uint16_t>(dm + 90);
            const auto fields = value<Address>(dm + 40);
            if (size < 40 || size > 4096 || !fields)
                return 0;
            for (unsigned i = 0; i < 23; ++i)
            {
                const auto field = value<Address>(fields + i * 8);
                if (!typed(field, 0x17a46b8) ||
                    string(value<Address>(field + 48)) != "PlayerDied_id_m8rhvs1z" ||
                    value<std::uint16_t>(field + 22) != 8)
                    continue;
                const auto offset = value<std::uint16_t>(field + 24);
                if (offset < 40 || offset + 8u > size)
                    return 0;
                const auto handle = value<Address>(data + offset), variable = resolve(handle);
                if ((value<unsigned>(handle) & 0xf0000000) == 0x40000000 &&
                    string(value<Address>(handle + 8)) == "PlayerDied" &&
                    string(value<Address>(handle + 24)) == "gamevariables" &&
                    typed(variable, 0x155ebf0) && value<unsigned>(variable + 16) <= 2)
                    return handle;
            }
            return 0;
        }
        void vehicle_death_event(Address node, Address context)
        {
            const auto component = value<Address>(context), meta = value<Address>(component + 56);
            if (meta < 60584 || node != meta - 60584 + 51664 || !typed(node, 0x13bc390) ||
                !vehicle_death_level() || !bound_player_died() || component != vehicle_death_component())
                return;
            const auto message = value<Address>(node + 16);
            if (!typed(message, 0x17b6578) ||
                string(value<Address>(message + 16)) != "Crash_DamageManager_c.GraphEvent_id_smxibdqb_Definition")
                return;
            // vehicle deaths emit this event without setting the walking character's PlayerDied variable.
            record_death_change(false, true, false);
        }
        __declspec(noinline) bool request_vehicle_death(Address component)
        {
            const auto meta = value<Address>(component + 56);
            const auto handle = vehicle_death_handle(component);
            if (!component || meta < 60584 || !handle || !bound_player_died() || resolve(handle) != bound_player_died())
                return false;
            const auto slab = meta - 60584;
            // enter the vehicle's lethal-hit branch: it animates rider AND vehicle, then respawns them together.
            if (!typed(slab + 50680, 0x175dbd8) || !typed(slab + 50936, 0x175dbd8) ||
                !typed(slab + 51552, 0x175dbd8) || !typed(slab + 51624, 0x175dbd8) ||
                !typed(slab + 51664, 0x13bc390) || !typed(slab + 62160, 0x17473c8) ||
                value<Address>(slab + 50680 + 16) != slab + 50720 ||
                value<Address>(slab + 50680 + 32) != slab + 50936 ||
                string(value<Address>(slab + 50720 + 24)) != "Ride_Jetski_Death_Mine" ||
                value<Address>(slab + 51624 + 32) != slab + 51664)
                return false;
            alignas(8) std::array<unsigned char, 72> method{};
            if (!read(slab + 62160, method) || value<Address>(slab + 62160 + 16) != slab + 62184 ||
                value<unsigned>(slab + 62184 + 16) != 1 || value<unsigned>(slab + 62184 + 20) != 1)
                return false;
            const auto address = reinterpret_cast<Address>(method.data());
            const auto pointer = [&](unsigned offset, Address v) { std::memcpy(method.data() + offset, &v, 8); };
            pointer(16, address + 24);
            pointer(56, address + 64);
            pointer(64, slab + 50680);
            reinterpret_cast<void (*)(Address, Address, unsigned, Address)>(base() + 0xc988c0)(address, component, 0, 0);
            return true;
        }
        __declspec(noinline) bool request_scripted_death(Address actor)
        {
            if (vehicle_death_level())
                return request_vehicle_death(vehicle_death_component());
            const auto native_sender = c2_boss_portal_hook.installed ? c2_boss_portal_hook.original : base() + 0x767da0;
            std::array<unsigned char, 16> prefix{};
            if (!read(native_sender, prefix) || prefix != std::array<unsigned char, 16>{0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x57, 0x48,
                                                                               0x83, 0xec, 0x40, 0x48, 0x83, 0x79, 0x10, 0})
                throw std::runtime_error("DeathLink script-message sender differs");
            for (const auto component : list(value<Address>(actor + 24), 128))
            {
                const auto meta = value<Address>(component + 56), data = value<Address>(component + 16);
                if (!typed(component, 0x13bbbd0) || value<Address>(component + 24) != actor ||
                    !(value<unsigned>(component + 32) & 1) || (value<unsigned>(component + 32) & 24) ||
                    !(value<unsigned char>(component + 40) & 1) || !typed(meta, 0x1745d78) ||
                    string(value<Address>(meta + 16)) != "Scripts.Graph.Crash_DamageManager" ||
                    !typed(data, 0x13bbb10) || !death_handle || value<Address>(data + 80) != death_handle.load())
                    continue;
                for (const auto delegator : list(value<Address>(meta + 160), 128))
                {
                    const auto message = value<Address>(delegator + 24), target = value<Address>(delegator + 32);
                    if (!typed(delegator, 0x13bc268) || !typed(message, 0x17b6578) || !typed(target, 0x13bbcb8) ||
                        string(value<Address>(message + 16)) != "Crash_DamageManager_c.GraphEvent_id_pmrewwnd_Definition")
                        continue;
                    if (!field_at(message, "_sender", 32, 8) ||
                        !field_at(message, "GraphEvent_id_snc05c5g_Definition_Parameter", 56, 8) ||
                        !field_at(message, "Field_id_16ghylas", 64, 1))
                        throw std::runtime_error("DeathLink falling-death message layout differs");
                    // CrashFallingDeath runs the normal respawn script. 
                    // KillEntity leaves this reused actor at zero health so we can't use it :(
                    std::array<Address, 11> sender{base() + 0x13bc390, 0, message, target};
                    std::array<Address, 6> context{component};
                    reinterpret_cast<Address (*)(Address, Address)>(native_sender)(
                        reinterpret_cast<Address>(sender.data()), reinterpret_cast<Address>(context.data()));
                    return true;
                }
            }
            return false;
        }
        __declspec(noinline) void bind_player_death(Address actor)
        {
            const bool vehicle = vehicle_death_level();
            if (actor == last_actor && vehicle == last_vehicle_death && bound_player_died())
                return;
            last_vehicle_death = vehicle;
            if (vehicle)
            {
                if (actor != last_actor)
                    remote_death = false;
                last_actor = actor;
                death_handle = vehicle_death_handle(vehicle_death_component());
                if (death_handle && bound_player_died())
                {
                    if (!code_matches(0x4e5c60, std::array<unsigned char, 5>{0xe9, 0xab, 0xe9, 0xff, 0xff}))
                        throw std::runtime_error("Game PlayerDied getter differs");
                    death_hook.attach(0x502000, {0x48, 0x89, 0x5c, 0x24, 8, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x40}, reinterpret_cast<Address>(player_died_set));
                }
                return;
            }
            if (actor != last_actor)
            {
                last_actor = actor;
                remote_death = false;
            }
            death_handle = 0;
            for (const auto c : list(value<Address>(actor + 24), 128))
            {
                if (!typed(c, 0x13bbbd0) || value<Address>(c + 24) != actor || !(value<unsigned>(c + 32) & 1) ||
                    (value<unsigned>(c + 32) & 24) || !(value<unsigned char>(c + 40) & 1))
                    continue;
                const auto meta = value<Address>(c + 56), data = value<Address>(c + 16), dm = value<Address>(data + 32);
                // DamageManager's data holds the PlayerDied handle at +80 , component+56 identifies the graph
                if (!typed(meta, 0x1745d78) || string(value<Address>(meta + 16)) != "Scripts.Graph.Crash_DamageManager" ||
                    !typed(data, 0x13bbb10) || !dm || dm != value<Address>(meta + 176) || !field_at(dm, "PlayerDied", 80, 8))
                    continue;
                death_handle = value<Address>(data + 80);
                if (!bound_player_died())
                {
                    death_handle = 0;
                    continue;
                }
                if (!code_matches(0x4e5c60, std::array<unsigned char, 5>{0xe9, 0xab, 0xe9, 0xff, 0xff}))
                    throw std::runtime_error("Game PlayerDied getter differs");
                death_hook.attach(0x502000, {0x48, 0x89, 0x5c, 0x24, 8, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x40}, reinterpret_cast<Address>(player_died_set));
                return;
            }
        }
        const BoxSpec flying_specs[]{
#include "flying_crate_specs.inc"
        };
        bool flying_crate_actor(Address actor)
        {
            for (const auto c : list(value<Address>(actor + 24), 64))
            {
                if (!typed(c, 0x13bbbd0) || value<Address>(c + 24) != actor)
                    continue;
                const auto meta = value<Address>(c + 56);
                if (typed(meta, 0x1745d78) && string(value<Address>(meta + 16)) == "Scripts.Graph.common_Crate_Flying_Level_Count")
                    return true;
            }
            return false;
        }
        std::vector<Address> prefab_instances()
        {
            // same metadata-to-instance table as event_owners(), selecting prefab instances rather than event components
            const auto context = value<Address>(base() + 0x1ac1dd8), meta = value<Address>(base() + 0x1ac1830);
            const auto table = value<Address>(context + 120);
            const auto bytes = value<std::uint64_t>(table + 16) & 0xfffffffffffffULL;
            if (!context || !meta || !table || bytes != (value<std::uint64_t>(table + 32) & 0xfffffffffffffULL) || bytes > 65536 * 8 || bytes % 8)
                return {};
            const auto keys = value<Address>(table + 40), lists = value<Address>(table + 24);
            for (unsigned i = 0; i < bytes / 8; ++i)
                if (value<Address>(keys + i * 8) == meta)
                    return list(value<Address>(lists + i * 8), 16384);
            return {};
        }
        const BoxSpec *flying_spec(Address root, const std::string &level)
        {
            std::array<unsigned char, 12> position{};
            if (!typed(root, 0x1722e80) || !read(root + 32, position))
                return nullptr;
            for (const auto &spec : flying_specs)
                if (level == spec.level && position == spec.position)
                    return &spec;
            return nullptr;
        }
        Address flying_root(Address actor, const std::vector<Address> &prefabs, const std::string &level, const BoxSpec *&identity)
        {
            Address result = 0;
            identity = nullptr;
            for (const auto c : prefabs)
            {
                if (!typed(c, 0x1724358) || value<unsigned char>(c + 56) != 1)
                    continue;
                const auto children = list(value<Address>(c + 40), 128);
                if (std::find(children.begin(), children.end(), actor) == children.end())
                    continue;
                const auto root = value<Address>(c + 24);
                const auto spec = flying_spec(root, level);
                if (!spec)
                    continue;
                if (result && result != root)
                {
                    identity = nullptr;
                    return 0;
                }
                result = root;
                identity = spec;
            }
            return result;
        }
        Address crate_entity(Address anchor)
        {
            // AP identities belong to the invisible spawner, not its shared archetype
            Address spawned = 0;
            bool outlined = false;
            for (const auto c : list(value<Address>(anchor + 24), 64))
            {
                if (!typed(c, 0x13bbbd0) || value<Address>(c + 24) != anchor || !(value<unsigned>(c + 32) & 1) ||
                    (value<unsigned>(c + 32) & 24) || !(value<unsigned char>(c + 40) & 1))
                    continue;
                const auto meta = value<Address>(c + 56);
                if (!typed(meta, 0x1745d78))
                    continue;
                const auto name = string(value<Address>(meta + 16));
                if (name == "Scripts.Graph.common_Crate_Outline")
                {
                    // the template holds the outline. this handle owns the box revealed by its switch
                    outlined = true;
                    const auto fields = value<Address>(meta + 40);
                    const auto count = value<unsigned>(meta + 24);
                    const auto size = value<std::uint16_t>(meta + 90);
                    if (!fields || count > 256 || size < 64 || size > 4096)
                        continue;
                    for (unsigned i = 0; i < count; ++i)
                    {
                        const auto field = value<Address>(fields + i * 8);
                        const auto offset = value<std::uint16_t>(field + 24);
                        if (string(value<Address>(field + 48)) != "replacementSpawned" || value<std::uint16_t>(field + 22) != 8 ||
                            offset < 64 || offset + 8 > size)
                            continue;
                        const auto entity = resolve(value<Address>(c + offset));
                        if (typed(entity, 0x141d490) && (value<unsigned>(entity + 56) & 4))
                            return entity;
                    }
                }
                if (name != "Scripts.Graph.common_Spawner_Template" || !field_at(meta, "SpawnedEntity", 232, 8))
                    continue;
                const auto entity = resolve(value<Address>(c + 232));
                if (typed(entity, 0x141d490) && (value<unsigned>(entity + 56) & 4))
                    spawned = entity;
            }
            return outlined ? 0 : spawned;
        }
        Address crate_model(Address entity)
        {
            // Entity+176 is the model component. +32 verifies its owner and +40 resolves the render model.
            const auto model = value<Address>(entity + 176), ig = value<Address>(model + 40);
            // Bounce and TNT crates use skinned models with the same entity/model fields
            return (typed(model, 0x144bff0) || typed(model, 0x15aee18)) && value<Address>(model + 32) == entity && typed(ig, 0x1676cb8) ? ig : 0;
        }
        bool crate_identity(Address actor, const Box &box, Address position_owner = 0)
        {
            if (!event_owner(actor))
                return false;
            if (position_owner && position_owner != actor)
            {
                const auto spec = flying_spec(position_owner, box.level);
                return spec && box.name == std::string(spec->name) && flying_crate_actor(actor) &&
                       !std::memcmp(spec->position.data(), box.position, sizeof(box.position));
            }
            std::array<float, 3> position{};
            return read(actor + 32, position) && string(value<Address>(actor + 128)) == box.name &&
                   !std::memcmp(position.data(), box.position, sizeof(box.position));
        }
        const Box *selected_box(const std::multimap<std::string, const Box *> &selected, const Box &identity)
        {
            const auto range = selected.equal_range(identity.name);
            for (auto it = range.first; it != range.second; ++it)
                if (!std::strcmp(it->second->level, identity.level) &&
                    !std::memcmp(it->second->position, identity.position, sizeof(identity.position)))
                    return it->second;
            return nullptr;
        }
        std::vector<Crate> crates(const Shared &state, const std::string &zone)
        {
            const auto level = level_key(zone);
            struct Cached
            {
                Address address, position_owner;
                Box identity;
            };
            static std::vector<Cached> cache;
            static std::string cached_zone;
            static std::uint64_t next_scan = 0;
            std::multimap<std::string, const Box *> selected;
            for (unsigned i = 0; i < state.box_count; ++i)
                if (level == state.boxes[i].level)
                    selected.emplace(state.boxes[i].name, &state.boxes[i]);
            const auto now = GetTickCount64();
            if (zone != cached_zone || now >= next_scan)
            {
                cache.clear();
                cached_zone = zone;
                next_scan = now + 500;
                if (selected.empty())
                    return {};
                const bool flying = level == "L317" || level == "L324" || level == "L330";
                const auto prefabs = flying ? prefab_instances() : std::vector<Address>{};
                std::map<const Box *, std::vector<std::pair<Address, Address>>> matches;
                for (const auto actor : event_owners())
                {
                    auto name = string(value<Address>(actor + 128));
                    Address position_owner = actor;
                    if (flying && flying_crate_actor(actor))
                    {
                        const BoxSpec *spec = nullptr;
                        position_owner = flying_root(actor, prefabs, level, spec);
                        if (!position_owner)
                            continue;
                        name = spec->name;
                    }
                    const auto range = selected.equal_range(name);
                    for (auto found = range.first; found != range.second; ++found)
                    {
                        if (!crate_identity(actor, *found->second, position_owner))
                            continue;
                        const auto pair = std::make_pair(actor, position_owner);
                        auto &owners = matches[found->second];
                        if (std::find(owners.begin(), owners.end(), pair) == owners.end())
                            owners.push_back(pair);
                    }
                }
                for (const auto &[identity, actors] : matches)
                    if (actors.size() == 1)
                        cache.push_back({actors.front().first, actors.front().second, *identity});
            }
            std::vector<Crate> result;
            for (const auto &c : cache)
            {
                const auto box = selected_box(selected, c.identity);
                if (!box || !crate_identity(c.address, *box, c.position_owner))
                    continue;
                const auto entity = crate_entity(c.address), model = crate_model(entity);
                if (entity && model)
                    result.push_back({c.address, entity, model, *box});
            }
            return result;
        }
        bool tint_api()
        {
            // guard tint creation/application and reference helpers before passing engine objects to these RVAs
            const std::pair<Address, std::array<unsigned char, 12>> guards[]{
                {0x882b0, {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8d, 0x15}},
                {0xacd1c0, {0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83}},
                {0xaf7370, {0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b}},
                {0xdda396, {0xc7, 0x41, 0x08, 0x01, 0x00, 0x00, 0x00, 0x4d, 0x85, 0xc0, 0x74, 0x0d}},
                {0xddde3c, {0x81, 0x66, 0x08, 0x00, 0x00, 0xf8, 0xff, 0x48, 0x8b, 0xce, 0xe8, 0xc5}},
                {0xdf0b40, {0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf2, 0x48}},
                {0xdf0e00, {0x48, 0x85, 0xc9, 0x74, 0x09, 0x48, 0x83, 0xc1, 0x08, 0xe9, 0xa2, 0x49}},
                {0xdf0e10, {0x48, 0x85, 0xc9, 0x0f, 0x85, 0x87, 0xcf, 0xfe, 0xff, 0xc3, 0xcc, 0xcc}}};
            for (const auto &[rva, bytes] : guards)
                if (value<std::array<unsigned char, 12>>(base() + rva) != bytes)
                    return false;
            const auto meta = value<Address>(base() + 0x1a55ff8);
            return value<Address>(meta + 56) == base() + 0x138da80 && string(value<Address>(meta + 16)) == "CFxTintBundle" &&
                   value<std::uint16_t>(meta + 90) == 80 && field_at(meta, "_multiplyColor", 32, 16) &&
                   field_at(meta, "_blendColor", 48, 16) && field_at(meta, "_blendEmissive", 64, 16);
        }
        void retain(Address object) { reinterpret_cast<void (*)(Address)>(base() + 0xdf0e00)(object); }
        void release(Address object) { reinterpret_cast<void (*)(Address)>(base() + 0xdf0e10)(object); }
        void model_handle(Address &handle, Address model) { reinterpret_cast<void (*)(Address *, Address)>(base() + 0xdf0b40)(&handle, model); }
        template <class T>
        bool write(Address at, const T &data)
        {
            SIZE_T done = 0;
            return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void *>(at), &data, sizeof(data), &done) && done == sizeof(data);
        }
        constexpr std::array<float, 4> blue_tint{1, 1, 1, 1};
        constexpr std::array<float, 4> default_tint{1, 1, 1, 1}, default_blend{1, 1, 1, 0}, default_emissive{1, 0, 0, 0};
        bool marker_tint(const TintedModel &t)
        {
            return typed(t.bundle, 0x138da80) && value<std::array<float, 4>>(t.bundle + 32) == blue_tint &&
                   value<std::array<float, 4>>(t.bundle + 48) == tint_blend(t.color, t.opacity) && value<std::array<float, 4>>(t.bundle + 64) == default_emissive;
        }
        bool has_tint(const TintedModel &t)
        {
            // the model pool invalidates registered handles before recycling even a retained model
            if (!t.model || resolve(t.handle) != t.model || !typed(t.model, 0x1676cb8) || !t.bundle)
                return false;
            const auto bundles = list(value<Address>(t.model + 224), 64);
            return std::find(bundles.begin(), bundles.end(), t.bundle) != bundles.end();
        }
        bool clear_tint(const TintedModel &t)
        {
            // remove only our custom bundle. the engine owns the list and any other effects on this model
            // TNT/pickup effects can adopt this bundle, gotta leave their changed values and ownership alone
            if (!has_tint(t) || !marker_tint(t))
                return false;
            reinterpret_cast<void (*)(Address, Address)>(base() + 0xaf7370)(t.model, t.bundle);
            return !has_tint(t);
        }
        void restore_tints(bool game_thread)
        {
            if (game_thread)
            {
                for (auto &t : tinted_models)
                {
                    clear_tint(t);
                    release(t.bundle);
                    model_handle(t.handle, 0);
                }
            }
            else
                for (const auto &t : tinted_models)
                    if (marker_tint(t))
                    {
                        write(t.bundle + 32, default_tint);
                        write(t.bundle + 48, default_blend);
                    }
            tinted_models.clear();
        }
        void update_tints(const Shared &state, const std::string &zone)
        {
            std::lock_guard lock(crate_mutex);
            static std::uint64_t next_update = 0;
            static std::string updated_zone;
            static float updated_opacity = 0;
            const bool enabled = state.visible && state.marker_mode == MarkersTint && state.tint_opacity != 0 && !level_key(zone).empty();
            const auto now = GetTickCount64();
            // retained bundles and registered handles survive between refreshes, dont reuse resolved targets!
            if (enabled && zone == updated_zone && state.tint_opacity == updated_opacity && now < next_update)
                return;
            next_update = enabled ? now + 100 : 0;
            updated_zone = zone;
            updated_opacity = state.tint_opacity;
            if (!enabled && tinted_models.empty())
                return;
            if (!tint_api())
            {
                restore_tints(false);
                return;
            }
            if (!enabled)
            {
                restore_tints(true);
                return;
            }
            const auto targets = crates(state, zone);
            std::set<std::pair<Address, unsigned>> selected_models;
            for (const auto &c : targets)
                selected_models.emplace(c.model, c.identity.color);
            for (auto it = tinted_models.begin(); it != tinted_models.end();)
            {
                if (!has_tint(*it) || !marker_tint(*it) || it->opacity != state.tint_opacity || !selected_models.count({it->model, it->color}))
                {
                    clear_tint(*it);
                    release(it->bundle);
                    model_handle(it->handle, 0);
                    it = tinted_models.erase(it);
                }
                else
                    ++it;
            }
            for (const auto &c : targets)
            {
                if (box_color_index(c.identity.color) >= 5 || tinted_models.size() >= max_boxes)
                    continue;
                const auto bundles = list(value<Address>(c.model + 224), 64);
                if (bundles.size() >= 64 || std::any_of(bundles.begin(), bundles.end(), [](Address b)
                                                        { return typed(b, 0x138da80); }))
                    continue;
                // game effects mutate tint bundles, so every model needs its own instance
                const auto bundle = reinterpret_cast<Address (*)(Address)>(base() + 0x882b0)(0);
                if (!typed(bundle, 0x138da80))
                    return;
                // the factory's count of 1 means no retained owner. keep our own reference across game object removal
                retain(bundle);
                if (value<std::array<float, 4>>(bundle + 32) != default_tint || value<std::array<float, 4>>(bundle + 48) != default_blend ||
                    value<std::array<float, 4>>(bundle + 64) != default_emissive || !write(bundle + 32, blue_tint) || !write(bundle + 48, tint_blend(c.identity.color, state.tint_opacity)))
                {
                    release(bundle);
                    return;
                }
                // engine drawing applies model bundles, draws, then restores each bundle's defaults
                // never put FX uniforms in a material: their GPU state survives/bleeds into scenery draws
                // a direct model retain survives engine object-pool reset and later releases a recycled object
                Address handle = 0;
                model_handle(handle, c.model);
                if (resolve(handle) != c.model)
                {
                    model_handle(handle, 0);
                    release(bundle);
                    continue;
                }
                reinterpret_cast<void (*)(Address, Address)>(base() + 0xacd1c0)(c.model, bundle);
                if (has_tint({c.model, bundle, c.identity.color, handle, state.tint_opacity}))
                    tinted_models.push_back({c.model, bundle, c.identity.color, handle, state.tint_opacity});
                else
                {
                    release(bundle);
                    model_handle(handle, 0);
                }
            }
        }
        bool crate_corners(Address model, LiveBox &box)
        {
            // instance bounds include animation padding. use the mesh's local bounds for outlines so they align properly.
            const auto mesh = value<Address>(model + 192);
            Matrix transform{};
            std::array<float, 4> minimum{}, maximum{};
            if (!typed(model, 0x1676cb8) || !typed(mesh, 0x1676c18) ||
                !read(model + 48, transform) || !read(mesh + 32, minimum) || !read(mesh + 48, maximum) ||
                value<Address>(model + 192) != mesh ||
                !std::all_of(transform.begin(), transform.end(), [](float f)
                             { return std::isfinite(f); }) ||
                std::abs(transform[15] - 1) > 0.001f)
                return false;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (!std::isfinite(minimum[axis]) || !std::isfinite(maximum[axis]) ||
                    minimum[axis] >= maximum[axis] || maximum[axis] - minimum[axis] > 10000)
                    return false;
            // model bounds are in model space. crates start at their base, not their center.
            for (unsigned i = 0; i < 8; ++i)
            {
                const float x = (i & 1) ? maximum[0] : minimum[0], y = (i & 2) ? maximum[1] : minimum[1], z = (i & 4) ? maximum[2] : minimum[2];
                for (unsigned axis = 0; axis < 3; ++axis)
                    box.corners[i][axis] = x * transform[axis] + y * transform[4 + axis] + z * transform[8 + axis] + transform[12 + axis];
            }
            return true;
        }
        Address c3_button_refresh_method(Address component, Address entity, bool layout = false)
        {
            if (!typed(component, 0x13bbbd0) || value<Address>(component + 24) != entity ||
                !(value<unsigned>(component + 32) & 1) || (value<unsigned>(component + 32) & 24) ||
                !(value<unsigned char>(component + 40) & 1) || !event_owner(entity))
                return 0;
            const auto meta = value<Address>(component + 56);
            if (!typed(meta, 0x1745d78) || string(value<Address>(meta + 16)) != "Scripts.Graph.common_C3_Hub_Portal_Button" ||
                value<std::uint16_t>(meta + 90) != 144 || meta < 42340)
                return 0;
            const auto slab = meta - 42340, method = slab + (layout ? 52556 : 51828), nodes = method + 24, entry = nodes + 48;
            // the two compiled methods refresh visibility or arrange the buttons. validate their one-entry node lists.
            const auto route = value<Address>(entry + 48);
            std::array<Address, 4> bindings{};
            if (!typed(method, 0x17473c8) || value<Address>(method + 16) != nodes || !typed(nodes, 0x1746090) ||
                value<unsigned>(nodes + 16) != 1 || value<unsigned>(nodes + 20) != 1 ||
                (value<std::uint64_t>(nodes + 24) & 0xfffffffffffffULL) != 8 || value<Address>(nodes + 32) != nodes + 40 ||
                value<Address>(nodes + 40) != entry || !typed(entry, 0x1746848) || !typed(route, layout ? 0x17592d8 : 0x1759220) ||
                value<Address>(entry + 24) || value<Address>(entry + 32) || value<Address>(entry + 40) ||
                value<unsigned>(entry + 56) != 4 || !read(value<Address>(entry + 72), bindings) ||
                std::any_of(bindings.begin(), bindings.end(), [](Address p)
                            { return p != 0; }))
                return 0;
            if (layout)
            {
                if (route != slab + 52780)
                    return 0;
            }
            else if (value<Address>(slab + 56556 + 48) != route || value<Address>(slab + 55644 + 48) != route)
                return 0;
            return method;
        }
        Address c3_layout_target(Address component)
        {
            const auto meta = value<Address>(component + 56), data = value<Address>(component + 16), dm = value<Address>(data + 32);
            if (!typed(data, 0x13bbb10) || !dm || dm != value<Address>(meta + 176) ||
                !field_at(dm, "igEntity_id_x9nfoby0_metaField", 96, 8))
                return 0;
            const auto target = resolve(value<Address>(data + 96));
            return typed(target, 0x1722e80) ? target : 0;
        }
        bool c3_static_trigger(Address component, Address entity)
        {
            const auto flags = value<unsigned>(component + 32);
            const auto data = value<Address>(component + 16);
            if (!typed(component, 0x148d9e8) || value<Address>(component + 24) != entity || !event_owner(entity) ||
                !(flags & 1) || (flags & 24) || !(value<unsigned char>(component + 40) & 1) ||
                !typed(data, 0x148d928) || value<unsigned char>(data + 68) != 1 ||
                value<unsigned char>(component + 0x670) != 0 || value<unsigned char>(component + 0x671) != 1)
                return false;
            const auto manager = value<Address>(component + 64);
            const auto index = value<unsigned>(component + 56);
            const auto count = value<unsigned>(manager + 44) & 0x3fffffff;
            const auto records = value<Address>(manager + 32);
            return manager && records && count <= 16384 && index < count &&
                   (value<unsigned char>(records + index * 144 + 64) & 3);
        }
        Address c3_platform_spawn_method(Address component, Address entity)
        {
            if (!typed(component, 0x13bbbd0) || value<Address>(component + 24) != entity ||
                !(value<unsigned>(component + 32) & 1) || (value<unsigned>(component + 32) & 24) ||
                !(value<unsigned char>(component + 40) & 1) || !event_owner(entity) ||
                !(value<unsigned>(entity + 56) & 0x100))
                return 0;
            const auto meta = value<Address>(component + 56), data = value<Address>(component + 16);
            if (!typed(meta, 0x1745d78) || string(value<Address>(meta + 16)) != "Scripts.Graph.common_Spawner_Template" ||
                value<std::uint16_t>(meta + 90) != 344 || meta < 45808 || !typed(data, 0x13bbb10) ||
                value<Address>(data + 32) != value<Address>(meta + 176) || !value<Address>(data + 32) ||
                !field_at(meta, "SpawnedEntity", 232, 8) || !field_at(meta, "isSpawningEnabled", 69, 1) ||
                !field_at(meta, "used", 67, 1) || !field_at(meta, "pendingUsed", 66, 1) ||
                value<unsigned char>(component + 69) != 1 || value<unsigned char>(component + 67) ||
                value<unsigned char>(component + 66) || resolve(value<Address>(component + 232)))
                return 0;
            const auto method = meta - 45808 + 67320, nodes = method + 24, entry = nodes + 48;
            // spawner method is slab-relative. only call its verified entry when no platform entity has spawned yet!
            std::array<Address, 4> bindings{};
            if (!typed(method, 0x17473c8) || value<Address>(method + 16) != nodes || !typed(nodes, 0x1746090) ||
                value<unsigned>(nodes + 16) != 1 || value<unsigned>(nodes + 20) != 1 ||
                (value<std::uint64_t>(nodes + 24) & 0xfffffffffffffULL) != 8 || value<Address>(nodes + 32) != nodes + 40 ||
                value<Address>(nodes + 40) != entry || !typed(entry, 0x1746848) ||
                value<Address>(entry + 24) || value<Address>(entry + 32) || value<Address>(entry + 40) ||
                value<Address>(entry + 48) != meta - 45808 + 67544 || !typed(value<Address>(entry + 48), 0x13bc390) ||
                value<unsigned>(entry + 56) != 4 || !read(value<Address>(entry + 72), bindings) ||
                std::any_of(bindings.begin(), bindings.end(), [](Address p)
                            { return p != 0; }))
                return 0;
            return method;
        }
        bool c3_platform_idle(const C3PlatformProbe &platform)
        {
            for (unsigned i = 0; i < 2; ++i)
            {
                unsigned movers = 0, spawners = 0;
                for (const auto component : list(value<Address>(platform.entities[i] + 24), 64))
                {
                    if (!typed(component, 0x13bbbd0) || value<Address>(component + 24) != platform.entities[i])
                        continue;
                    const auto meta = value<Address>(component + 56);
                    if (!typed(meta, 0x1745d78))
                        continue;
                    const auto name = string(value<Address>(meta + 16));
                    if (name == "Scripts.Graph.common_Path_Platform_Mover")
                    {
                        if (value<std::uint16_t>(meta + 90) != 337 ||
                            !field_at(meta, "playerOnPlatform_id_96cr4i6a", 64, 1) ||
                            !field_at(meta, "InternalStore_sliderData_id_mt251ryr", 160, 8) ||
                            !field_at(meta, "InternalStore_updateNodeUpdater_id_1rl5k748", 224, 8) ||
                            value<unsigned char>(component + 64) || value<Address>(component + 160) || value<Address>(component + 224))
                            return false;
                        ++movers;
                    }
                    else if (name == "Scripts.Graph.common_Spawner_Template")
                    {
                        if (value<std::uint16_t>(meta + 90) != 344 ||
                            !field_at(meta, "SpawnedEntity", 232, 8) || !field_at(meta, "used", 67, 1) ||
                            !field_at(meta, "pendingUsed", 66, 1) || value<Address>(component + 232) ||
                            value<unsigned char>(component + 67) || value<unsigned char>(component + 66))
                            return false;
                        ++spawners;
                    }
                }
                if (movers != 1 || spawners != 1)
                    return false;
            }
            return true;
        }
        void update_c3_buttons(Shared &state, const std::string &zone)
        {
            static std::uint64_t next = 0, pending_triggers = 0;
            static std::set<std::pair<Address, Address>> platform_spawn_attempts;
            static std::pair<Address, Address> initialized_platform{};
            if (hub_campaign(zone) != 3)
            {
                next = 0;
                pending_triggers = 0;
                platform_spawn_attempts.clear();
                initialized_platform = {};
                return;
            }
            if (GetTickCount64() < next)
                return;
            next = GetTickCount64() + (initialized_platform.first ? 250 : 16);
            const auto refresh_triggers = pending_triggers;
            pending_triggers = 0;
            if (!code_matches(0xc988c0, std::array<unsigned char, 16>{0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x6b, 0x18, 0x56,
                                                                      0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x70}))
                throw std::runtime_error("Button script-method executor differs");
            unsigned refreshed = 0, arranged = 0, trigger_refreshes = 0;
            // resolve current owners on this thread. never queue spawned pointers across a level load
            for (const auto entity : event_owners())
            {
                const auto name = string(value<Address>(entity + 128));
                if (name == "Generic_Path_Platform_Start_FadeOut" || name == "Generic_Path_Platform_End_FadeIn")
                {
                    const auto components = list(value<Address>(entity + 24), 64);
                    const auto offset = name == "Generic_Path_Platform_Start_FadeOut" ? 372300u : 365140u;
                    const auto cap = entity >= offset ? entity - offset + 364580 : 0;
                    const bool unlocked = typed(cap, 0x14c8ef8) && string(value<Address>(cap + 128)) == "FloorCap" &&
                                          !(value<unsigned>(cap + 56) & 0x100);
                    // enabling a previously disabled owner can leave its spawner empty after hub reload
                    const bool moving = std::any_of(components.begin(), components.end(), [](Address c)
                                                    { return typed(c, 0x13bbbd0) && string(value<Address>(value<Address>(c + 56) + 16)) == "Scripts.Graph.common_Path_Platform_Mover" &&
                                                             (value<unsigned char>(c + 64) || value<Address>(c + 160) || value<Address>(c + 224)); });
                    if (unlocked && !moving)
                        for (const auto component : components)
                        {
                            const auto method = c3_platform_spawn_method(component, entity);
                            if (method && platform_spawn_attempts.emplace(entity, component).second)
                            {
                                reinterpret_cast<void (*)(Address, Address, unsigned, Address)>(base() + 0xc988c0)(method, component, 0, 0);
                                std::cout << "Crash 3 secret platform spawn requested through the game script.\n"
                                          << std::flush;
                            }
                        }
                }
                const auto index = c3_button_index(name);
                if (index < 0)
                    continue;
                const unsigned char desired = (state.c3_buttons_active >> index) & 1;
                const auto components = list(value<Address>(entity + 24), 64);
                bool moved = false, visibility_changed = false;
                for (const auto component : components)
                {
                    if (name == "C3_Hub_Portal_Button01" &&
                        typed(component, 0x13bbbd0) && value<Address>(component + 24) == entity &&
                        (value<unsigned>(component + 32) & 1) && !(value<unsigned>(component + 32) & 24) &&
                        (value<unsigned char>(component + 40) & 1) &&
                        string(value<Address>(value<Address>(component + 56) + 16)) == "Scripts.Graph.common_C3_Hub_Portal_Button")
                    {
                        try
                        {
                            const auto platform = probe_c3_platform_at(GetCurrentProcess(), entity, "L301", false);
                            const auto meta = value<Address>(component + 56);
                            const auto route = value<Address>(meta - 42340 + 51900 + 48);
                            const auto button = probe_c3_button_events_at(GetCurrentProcess(), entity, "L301", route);
                            if (initialize_c3_platform(button, platform, initialized_platform))
                                std::cout << "Crash 3 secret elevator unlocked automatically.\n" << std::flush;
                        }
                        catch (const std::runtime_error &)
                        {
                            // portal discovery can still be loading or replacing the shared graph, retry after validation succeeds
                            if (state.c3_buttons_ready)
                                throw;
                        }
                    }
                    if (!state.c3_buttons_ready)
                        continue;
                    const auto method = c3_button_refresh_method(component, entity);
                    if (!method)
                        continue;
                    const auto target = c3_layout_target(component), layout_method = c3_button_refresh_method(component, entity, true);
                    std::array<float, 3> position{}, destination{};
                    if (target && layout_method && read(entity + 32, position) && read(target + 32, destination) && position != destination &&
                        std::all_of(destination.begin(), destination.end(), [](float f)
                                    { return std::isfinite(f); }) &&
                        std::isfinite(position[0]) && std::isfinite(position[1]) && std::isfinite(position[2]) &&
                        std::abs(position[0] - destination[0]) < 1024 && std::abs(position[1] - destination[1]) < 1024 && std::abs(position[2] - destination[2]) < 1024)
                    {
                        reinterpret_cast<void (*)(Address, Address, unsigned, Address)>(base() + 0xc988c0)(layout_method, component, 0, 0);
                        ++arranged;
                        moved = true;
                    }
                    if (read(entity + 32, position))
                        for (unsigned i = 0; i < state.entrance_count; ++i)
                            if (state.entrances[i].c3_button == index)
                            {
                                std::memcpy(state.entrances[i].position, position.data(), 12);
                                break;
                            }
                    const auto enabled = value<unsigned char>(component + 128), shown = value<unsigned char>(component + 88);
                    if (enabled > 1 || shown > 1)
                        throw std::runtime_error("Invalid button visibility state");
                    if (enabled == desired && shown == desired)
                        continue;
                    reinterpret_cast<void (*)(Address, Address, unsigned, Address)>(base() + 0xc988c0)(method, component, 0, 0);
                    ++refreshed;
                    visibility_changed = true;
                }
                const auto bit = std::uint64_t(1) << index;
                // show starts the trigger later. retain only its catalogue bit until registration settles.
                if (desired && (moved || visibility_changed || (refresh_triggers & bit)))
                {
                    bool applied = false;
                    if (!moved && !visibility_changed)
                        for (const auto component : components)
                            if (c3_static_trigger(component, entity))
                            {
                                if (!code_matches(0x5912a0, std::array<unsigned char, 16>{0x40, 0x53, 0x48, 0x81, 0xec, 0xc0, 0, 0,
                                                                                          0, 0x80, 0xb9, 0x70, 0x06, 0, 0, 0}))
                                    throw std::runtime_error("Game trigger-refresh function differs");
                                reinterpret_cast<void (*)(Address)>(base() + 0x5912a0)(component);
                                ++trigger_refreshes;
                                applied = true;
                            }
                    if (!applied)
                        pending_triggers |= bit;
                }
            }
            if (refreshed)
                std::cout << "Crash 3 button visibility refreshed: " << refreshed << ".\n"
                          << std::flush;
            if (arranged)
                std::cout << "Crash 3 six-button layout applied: " << arranged << ".\n"
                          << std::flush;
            if (trigger_refreshes)
                std::cout << "Crash 3 moved trigger bindings refreshed: " << trigger_refreshes << ".\n"
                          << std::flush;
        }

    }
    std::uintptr_t crate_identity_owner(std::uintptr_t owner, const char **name)
    {
        if (!owner || !name)
            return 0;
        if (!flying_crate_actor(owner))
            return owner;
        const auto world = value<Address>(base() + 0x1a64690);
        const auto key = level_key(string(value<Address>(world + 184)));
        const BoxSpec *spec = nullptr;
        const auto root = flying_root(owner, prefab_instances(), key, spec);
        if (root)
            *name = spec->name;
        return root ? root : owner;
    }
    void inherit_license()
    {
        if (!code_matches(0x4fdb50, std::array<unsigned char, 20>{0x48, 0x8b, 0xc4, 0x55, 0x56, 0x41, 0x55, 0x41, 0x57,
                                                                  0x48, 0x8d, 0x68, 0xb8, 0x48, 0x81, 0xec, 0x28, 0x01, 0x00, 0x00}))
            throw std::runtime_error("Game license-acceptance setter differs");
        // installed while startup waits at the save gate
        license_hook.attach(0x7550c0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20},
                            reinterpret_cast<Address>(license_query));
    }
    void preserve_shared_ui_audio()
    {
        // the common resource loader serves both sound banks and their samples. installed before the first hub loads.
        ui_audio_hook.attach(0x307ee0, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x55, 0x57, 0x41, 0x56},
                             reinterpret_cast<Address>(load_ui_audio));
    }
    void configure_hub_access(const std::array<unsigned, 3> &areas)
    {
        if ((areas[0] & ~7u) || (areas[1] & ~31u) || (areas[2] & ~31u))
            throw std::runtime_error("Invalid hub area permissions");
        hub_areas.store(areas[0] | (areas[1] << 5) | (areas[2] << 10), std::memory_order_relaxed);
        c2_boss_portal_hook.attach(0x767da0, {0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x57, 0x48,
                                           0x83, 0xec, 0x40, 0x48, 0x83, 0x79, 0x10, 0},
                                   reinterpret_cast<Address>(c2_boss_portal_event));
        // copy each complete leaf function, including its internal short branches and returns.
        hub_bool_hook.attach(0xc9c130, {0x48, 0x8b, 0x41, 0x18, 0x48, 0x85, 0xc0, 0x74, 0x11, 0x48, 0x8b, 0x0a,
                                      0x48, 0x85, 0xc9, 0x74, 0x09, 0x0f, 0xb7, 0x40, 0x18, 0x0f, 0xb6, 0x04,
                                      0x08, 0xc3, 0x32, 0xc0, 0xc3}, reinterpret_cast<Address>(hub_bool_field));
        hub_int_hook.attach(0xc9e820, {0x4c, 0x8b, 0x02, 0x4d, 0x85, 0xc0, 0x74, 0x12, 0x48, 0x8b, 0x41, 0x18,
                                     0x48, 0x85, 0xc0, 0x74, 0x09, 0x0f, 0xb7, 0x40, 0x18, 0x42, 0x8b, 0x04,
                                     0x00, 0xc3, 0x33, 0xc0, 0xc3}, reinterpret_cast<Address>(hub_int_field));
        if (!hub_gate_query_hook.installed &&
            !code_matches(0xcb6db0, std::array<unsigned char, 10>{0x48, 0x85, 0xd2, 0x74, 0x36, 0x48, 0x89, 0x5c, 0x24, 8}))
            throw std::runtime_error("Hub graph dispatcher differs from the verified build");
        hub_gate_query_hook.attach(0x831da0, {0x48, 0x89, 0x5c, 0x24, 8, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                                            0x48, 0x83, 0xec, 0x20}, reinterpret_cast<Address>(hub_gate_query));
        if (!c2_secret_entry_hook.installed &&
            !code_matches(0xcb3a60, std::array<unsigned char, 13>{0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x49, 0x10,
                                                               0x48, 0x85, 0xc9, 0x74, 0x2c}))
            throw std::runtime_error("C2 portal gate-open method differs from the verified build");
        c2_secret_entry_hook.attach(0xcabcc0, {0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57,
                                              0x48, 0x83, 0xec, 0x20}, reinterpret_cast<Address>(c2_secret_entry_gate));
    }
    void configure_movement(const std::map<std::string, unsigned> &styles)
    {
        if (!styles.empty())
        {
            // installed at the startup save gate, before the first player runs its graph's OnStart
            if (!code_matches(0xcb6db0, std::array<unsigned char, 10>{0x48, 0x85, 0xd2, 0x74, 0x36, 0x48, 0x89, 0x5c, 0x24, 0x08}))
                throw std::runtime_error("Movement graph dispatcher differs from the verified build");
            if (!code_matches(0x47cfa0, std::array<unsigned char, 15>{0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20}))
                throw std::runtime_error("Movement attack event lookup differs from the verified build");
            if (!movement_input_code)
            {
                if (!code_matches(0x4c06ee, std::array<unsigned char, 4>{0x0f, 0x92, 0xc0, 0xc3}))
                    throw std::runtime_error("Movement input getter differs from the verified build");
                const auto code = movement_input_stub(reinterpret_cast<Address>(&blocked_crouch_controller));
                const auto memory = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (!memory)
                    throw std::runtime_error("Cannot allocate movement input guard");
                std::memcpy(memory, code.data(), code.size());
                DWORD before = 0;
                if (!VirtualProtect(memory, code.size(), PAGE_EXECUTE_READ, &before))
                {
                    VirtualFree(memory, 0, MEM_RELEASE);
                    throw std::runtime_error("Cannot protect movement input guard");
                }
                FlushInstructionCache(GetCurrentProcess(), memory, code.size());
                movement_input_code = reinterpret_cast<Address>(memory);
            }
            movement_input_hook.attach(0x4c06e0, {0x48, 0x8b, 0x41, 0x10, 0x8b, 0xca, 0x48, 0x8b, 0x40, 0x10, 0x48, 0x0f, 0xa3, 0xc8},
                                       movement_input_code);
            // the attack selector returns 0..3 or 4 for no attack. walking slide uses TertiaryFadeIn.
            movement_attack_hook.attach(0x483670, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20},
                                        reinterpret_cast<Address>(movement_attack));
            // CSkillUpgradeFilter resolves its skill handle at +24 and applies the normal/inverted mode at +16
            movement_skill_hook.attach(0x4e9f60, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18},
                                       reinterpret_cast<Address>(movement_skill));
            movement_check_hook.attach(0x82f220, {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20},
                                       reinterpret_cast<Address>(movement_check));
        }
        std::lock_guard lock(movement_mutex);
        movement_styles = styles;
    }
    std::vector<LiveBox> live_boxes(const Shared &state, const std::string &zone)
    {
        if (!marker_bounds(state.marker_mode))
            return {};
        std::lock_guard lock(crate_mutex);
        std::vector<LiveBox> result;
        for (const auto &c : crates(state, zone))
        {
            LiveBox box;
            box.color = c.identity.color;
            if (!crate_corners(c.model, box))
                continue;
            result.push_back(box);
        }
        return result;
    }
    void update_gameplay(Shared &state)
    {
        const auto marker_world = value<Address>(base() + 0x1a64690);
        update_c2_secret_returns(hub_campaign(string(value<Address>(marker_world + 184))));
        update_c3_buttons(state, string(value<Address>(marker_world + 184)));
        update_collectibles(state, string(value<Address>(marker_world + 184)));
        update_tints(state, string(value<Address>(marker_world + 184)));
        const auto actor = player();
        vanilla_movement = state.vanilla_controls != 0;
        const auto world = value<Address>(base() + 0x1a64690);
        const auto key = level_key(string(value<Address>(world + 184)));
        // mixed vehicle levels retain normal controls until their active form is mapped
        const bool walking = actor && typed(value<Address>(actor + 424), 0x1589128) && !special_level(key);
        const auto desired = walking && !vanilla_movement ? assigned_movement() : 0;
        blocked_crouch_controller = walking && movement_style(desired, scene_campaign(), false) == 1
                                        ? value<Address>(actor + 424) : 0;
        active_style = 0;
        state.applied_style = 0;
        if (desired && movement_check_hook.installed && movement_input_hook.installed && movement_attack_hook.installed && movement_skill_hook.installed)
        {
            bool crouch = false;
            for (const auto component : list(value<Address>(actor + 24), 128))
                if (typed(component, 0x13bbbd0) && value<Address>(component + 24) == actor &&
                    (value<unsigned>(component + 32) & 1) && !(value<unsigned>(component + 32) & 24) &&
                    string(value<Address>(value<Address>(component + 56) + 16)) == "Scripts.Graph.Crash_CrouchCrawlSlide")
                    crouch = true;
            if (desired == 1 || crouch)
            {
                missing_crouch_actor = 0;
                active_style = desired;
                state.applied_style = desired;
            }
            else if (actor != missing_crouch_actor)
            {
                missing_crouch_actor = actor;
                copy(state.error, "Crashsanity waiting for crouch/slide component. Re-enter the level.");
            }
        }
        if (key.empty() || !state.death_link)
        {
            last_actor = 0;
            death_handle = 0;
            remote_death = false;
            state.death_ack = state.death_request;
            return;
        }
        // scripted deaths keep positive actor health. observe PlayerDied before the actor disappears
        if (!actor)
            return;
        bind_player_death(actor);
        if (!code_matches(0x4df230, std::array<unsigned char, 16>{0x48, 0x89, 0x5c, 0x24, 8, 0x57, 0x48, 0x83,
                                                                  0xec, 0x20, 0x8b, 0xb9, 0xf8, 0, 0, 0}))
            throw std::runtime_error("DeathLink health getter differs");
        const auto health = reinterpret_cast<int (*)(Address)>(base() + 0x4df230)(actor);
        if (health < 0 || health > 10000)
            return;
        const auto died_variable = bound_player_died();
        const bool scripted_death = died_variable && reinterpret_cast<unsigned char (*)(Address)>(base() + 0x4e5c60)(died_variable);
        const bool alive = health > 0 && !(value<unsigned char>(actor + 288) & 4) && !scripted_death;
        if (!alive)
            state.death_ack = state.death_request;
        if (remote_death && alive && GetTickCount64() > remote_deadline)
        {
            remote_death = false;
            copy(state.error, "DeathLink death was not observed after the game death script");
        }
        if (state.death_request != state.death_ack && alive && !remote_death && !state.console_open &&
            death_controls_ready(actor, scene().camera_active))
        {
            remote_death = true;
            remote_deadline = GetTickCount64() + 3000;
            if (request_scripted_death(actor))
                state.death_ack = state.death_request;
            else
            {
                remote_death = false;
                copy(state.error, "DeathLink waiting for the player's death script");
            }
        }
    }
    unsigned take_observed_deaths() { return observed_deaths.exchange(0); }
    void restore_gameplay(bool game_thread)
    {
        std::lock_guard lock(crate_mutex);
        restore_tints(game_thread);
        active_style = 0;
        // these guards remain active because an existing actor could retain its crouch component
        vanilla_movement = true;
        const auto actor = player(), controller = value<Address>(actor + 424);
        blocked_crouch_controller = actor && typed(controller, 0x1589128) && scene_campaign() == 1 ? controller : 0;
        death_handle = 0;
        death_hook.restore();
        observed_deaths = 0;
        last_actor = 0;
        remote_death = false;
        restore_collectible_hooks();
    }
    unsigned movement_campaign(std::uintptr_t manager)
    {
        const auto desired = active_style.load();
        return desired ? desired : reinterpret_cast<unsigned (*)(Address)>(base() + 0x131400)(manager);
    }
    unsigned movement_double_jump(unsigned owned)
    {
        if (!owned)
            return 0;
        const auto style = active_style.load();
        const auto manager = value<Address>(base() + 0x1a64690);
        const auto campaign = style ? 0 : manager ? reinterpret_cast<unsigned (*)(Address)>(base() + 0x131400)(manager)
                                                  : 0;
        return movement_double_jump_allowed(owned, style, campaign);
    }
}
