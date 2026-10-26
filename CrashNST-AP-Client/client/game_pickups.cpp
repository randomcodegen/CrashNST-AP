#include "game_pickups.h"
#include "game_runtime.h"
#include "function_hook.h"
#include "engine_memory.h"
#include <iostream>
#include <cmath>
#include <mutex>
#include <stdexcept>
#ifdef _MSC_VER
#include <intrin.h>
#endif
namespace ingame
{
    using namespace engine_memory;
    namespace
    {
        std::mutex collectible_mutex;
        std::vector<Collectible> collectible_config;
        std::map<std::string, unsigned> collectible_earned;
        thread_local bool collectible_grant_active = false;
        FunctionHook pending_hook, reward_hook, crystal_hook, relic_hook, relic_count_hook, collectible_query_hook, time_trial_hook;
        using TimeTrialResult = void (*)(int, Address, int *, int *, int *, unsigned char *, unsigned char *,
                                        unsigned char *, unsigned char *, int *);
        __declspec(noinline) void completed_time_trial(Address zone, int time)
        {
            if (collectible_grant_active || time < 0 || !typed(zone, 0x13d0fb0))
                return;
            const auto manager = value<Address>(base() + 0x1a5b690);
            const auto key = level_key(string(value<Address>(zone + 16)));
            const auto spec = collectible_spec(key.c_str());
            const auto world = value<Address>(base() + 0x1a64690);
            float sapphire = 0;
            if (resolve(value<Address>(manager + 72)) != zone || !spec || !spec->items[2] ||
                key != level_key(string(value<Address>(world + 184))) || !read(zone + 0x48, sapphire) ||
                !std::isfinite(sapphire) || sapphire <= 0 || !std::isfinite(sapphire * 100.0f) ||
                static_cast<float>(time) > sapphire * 100.0f)
                return;
            std::lock_guard lock(collectible_mutex);
            for (const auto &c : collectible_config)
                if (key == c.key)
                {
                    collectible_earned[key] |= spec->masks[2];
                    return;
                }
        }
        void time_trial_callback(int time, Address name, int *first, int *second, int *third,
                                 unsigned char *new_first, unsigned char *new_second, unsigned char *new_third,
                                 unsigned char *unplaced, int *rank)
        {
            const auto key = level_key(string(value<Address>(value<Address>(base() + 0x1a64690) + 184)));
            reinterpret_cast<TimeTrialResult>(time_trial_hook.original)(
                time, name, first, second, third, new_first, new_second, new_third, unplaced, rank);
            // relic awards only run for a new personal best. AP needs any qualifying completed run.
            const auto manager = value<Address>(base() + 0x1a5b690);
            const auto zone = resolve(value<Address>(manager + 72));
            if (level_key(string(value<Address>(zone + 16))) == key)
                completed_time_trial(zone, time);
        }
        std::pair<unsigned, unsigned> pickup_query(const std::string &key, unsigned mask)
        {
            std::lock_guard lock(collectible_mutex);
            for (const auto &c : collectible_config)
                if (key == c.key)
                {
                    const auto found = collectible_earned.find(key);
                    const auto earned = found == collectible_earned.end() ? 0 : found->second;
                    // unselected starting rewards must hide their pickup even before the game save row is updated
                    return {mask & ~c.selected, (collectible_observed(mask) &
                                                 (((c.checked | earned) & c.selected) | (c.owned & ~c.selected))) != 0};
                }
            return {mask, 0};
        }
        unsigned char has_rewarded_callback(Address zone, unsigned mask)
        {
            const auto caller = reinterpret_cast<Address>(
#ifdef _MSC_VER
                _ReturnAddress()
#else
                __builtin_return_address(0)
#endif
            );
            // only the game hasZoneCollectibleRewarded_VSC node uses pickup completion
            if (caller == base() + 0x831e00 && typed(zone, 0x13d0fb0))
            {
                const auto query = pickup_query(level_key(string(value<Address>(zone + 16))), mask);
                if (query.second)
                    return 1;
                if (!query.first)
                    return 0;
                mask = query.first;
            }
            return reinterpret_cast<unsigned char (*)(Address, unsigned)>(collectible_query_hook.original)(zone, mask);
        }
        bool blocked_relic(const std::string &key)
        {
            std::lock_guard lock(collectible_mutex);
            for (const auto &c : collectible_config)
                if (key == c.key)
                    return (c.blocked & 0x7000);
            return false;
        }
        void relic_callback(Address wrapper, Address descriptor, unsigned rank, int index)
        {
            if (!collectible_grant_active && rank > 0 && rank <= 3 && descriptor == value<Address>(base() + 0x1a58348))
            {
                const auto key = level_key(string(value<Address>(value<Address>(base() + 0x1a64690) + 184)));
                if (blocked_relic(key))
                {
                    const auto zones = list(value<Address>(value<Address>(base() + 0x1a5b690) + 40), 4096);
                    for (auto zone : zones)
                        if (typed(zone, 0x13d0fb0) && value<Address>(zone + 0x150) == wrapper &&
                            level_key(string(value<Address>(zone + 16))) == key)
                        {
                            std::lock_guard lock(collectible_mutex);
                            collectible_earned[key] |= 0x2000;
                            return;
                        }
                }
            }
            reinterpret_cast<void (*)(Address, Address, unsigned, int)>(relic_hook.original)(wrapper, descriptor, rank,
                                                                                             index);
        }
        void relic_count_callback(unsigned rank, int delta)
        {
            if (!collectible_grant_active &&
                blocked_relic(level_key(string(value<Address>(value<Address>(base() + 0x1a64690) + 184)))))
                return;
            reinterpret_cast<void (*)(unsigned, int)>(relic_count_hook.original)(rank, delta);
        }
        unsigned filtered_reward(Address zone, unsigned mask)
        {
            if (collectible_grant_active)
                return mask;
            if (!typed(zone, 0x13d0fb0))
                return mask;
            const auto key = level_key(string(value<Address>(zone + 16)));
            std::lock_guard lock(collectible_mutex);
            for (const auto &c : collectible_config)
                if (key == c.key)
                {
                    const auto spec = collectible_spec(c.key);
                    collectible_earned[key] |= collectible_observed(mask) & collectible_mask(*spec);
                    return mask & ~c.blocked;
                }
            return mask;
        }
        void pending_reward(Address zone, unsigned mask)
        {
            mask = filtered_reward(zone, mask);
            if (mask)
                reinterpret_cast<void (*)(Address, unsigned)>(pending_hook.original)(zone, mask);
        }
        void reward_callback(Address zone, unsigned mask)
        {
            mask = filtered_reward(zone, mask);
            if (mask)
                reinterpret_cast<void (*)(Address, unsigned)>(reward_hook.original)(zone, mask);
        }
        void crystal_callback(Address zone, bool collected)
        {
            if (!collected || filtered_reward(zone, 0x200))
                reinterpret_cast<void (*)(Address, bool)>(crystal_hook.original)(zone, collected);
        }
    } // namespace
    void restore_collectible_hooks()
    {
        {
            std::lock_guard lock(collectible_mutex);
            collectible_config.clear();
            collectible_earned.clear();
        }
        for (auto hook :
             {&pending_hook, &reward_hook, &crystal_hook, &relic_hook, &relic_count_hook, &collectible_query_hook, &time_trial_hook})
            try
            {
                hook->restore();
            }
            catch (const std::exception &e)
            {
                std::cerr << e.what() << ". Restart NST before reconnecting.\n";
            }
        // trampolines stay resident with the DLL so a late engine call can finish safely.
    }
    namespace
    {
        bool zone_session(Address zone, Address &wrapper, Address &row)
        {
            // user+136 selects the session. zone+0x150 holds its wrapper, with row pointers at +32 in eight-byte slots.
            if (!typed(zone, 0x13d0fb0))
                return false;
            const auto user = value<Address>(base() + 0x1a8c700);
            int index = -1;
            if (!user || !read(user + 136, index) || index < 0 || index >= 12)
                return false;
            wrapper = value<Address>(zone + 0x150);
            row = value<Address>(wrapper + 32 + index * 8);
            return wrapper && typed(row, 0x13d1828);
        }
        unsigned grant_collectibles(Address zone_address, Address wrapper, Address row, unsigned requested,
                                    Address descriptor, void (*set_relic)(Address, Address, unsigned, int),
                                    void (*count_relic)(unsigned, int))
        {
            struct GrantGuard
            {
                bool before = collectible_grant_active;
                GrantGuard()
                {
                    collectible_grant_active = true;
                }
                ~GrantGuard()
                {
                    collectible_grant_active = before;
                }
            } guard;
            const auto owned = value<unsigned>(row + 0x50), relic = value<unsigned>(row + 0x48);
            const auto crystal = value<unsigned char>(row + 0x4c);
            if (relic > 3 || crystal > 1)
                throw std::runtime_error("Local collectible session differs");
            const auto award = reinterpret_cast<void (*)(Address, unsigned)>(reward_hook.original);
            unsigned awards = 0;
            for (unsigned bit = 2; bit <= 256; bit <<= 1)
                if ((requested & bit) && !(owned & bit))
                {
                    award(zone_address, bit);
                    ++awards;
                }
            if (requested & 0x200)
            {
                if (!crystal)
                {
                    reinterpret_cast<void (*)(Address, bool)>(crystal_hook.original)(zone_address, true);
                    ++awards;
                }
                if (!(owned & 0x200))
                    award(zone_address, 0x200);
            }
            if (requested & 0x2000)
            {
                if (!relic)
                {
                    set_relic(wrapper, descriptor, 1, -1);
                    count_relic(1, 1);
                    ++awards;
                }
                if (!(owned & 0x7000))
                    award(zone_address, relic == 3 ? 0x1000 : relic == 2 ? 0x4000 : 0x2000);
            }
            return awards;
        }
    } // namespace
    void update_collectibles(Shared &state, const std::string &zone)
    {
        {
            std::lock_guard lock(collectible_mutex);
            collectible_config.assign(state.collectibles, state.collectibles + state.collectible_count);
            state.collectible_observation_count = 0;
            for (const auto &[key, mask] : collectible_earned)
                if (mask && state.collectible_observation_count < max_levels)
                {
                    auto &observation = state.collectible_observations[state.collectible_observation_count++];
                    copy(observation.key, key);
                    observation.mask = mask;
                }
        }
        if (!state.collectible_count)
        {
            if (pending_hook.installed || reward_hook.installed || crystal_hook.installed || time_trial_hook.installed)
                restore_collectible_hooks();
            return;
        }
        if (!code_matches(0x831dfb, std::array<unsigned char, 5>{0xe8, 0xf0, 0x9c, 0xf2, 0xff}) ||
            (!collectible_query_hook.installed &&
             !code_matches(0x75baf0, std::array<unsigned char, 15>{0x48, 0x89, 0x5c, 0x24, 8, 0x48, 0x89, 0x6c, 0x24,
                                                                   0x10, 0x48, 0x89, 0x74, 0x24, 0x18})))
            throw std::runtime_error("Collectible script query differs");
        if ((!relic_hook.installed &&
             !code_matches(0x73c780, std::array<unsigned char, 16>{0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec,
                                                                   0x30, 0x45, 0x8b, 0xf8, 0x4c, 0x8b, 0xe2})) ||
            (!relic_count_hook.installed &&
             !code_matches(0x75d150, std::array<unsigned char, 16>{0x48, 0x89, 0x5c, 0x24, 8, 0x57, 0x48, 0x83, 0xec,
                                                                   0x20, 0x8b, 0xfa, 0x8b, 0xd9, 0xe8, 0xad})))
            throw std::runtime_error("Collectible setters differ. Game reward grants stopped");
        // hook function prologues. FunctionHook replays these bytes in its trampoline.
        pending_hook.attach(0x73ec30,
                            {0x48, 0x89, 0x5c, 0x24, 8, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xda, 0x48, 0x8b, 0xf9},
                            reinterpret_cast<Address>(pending_reward));
        reward_hook.attach(0x73ec70,
                           {0x48, 0x89, 0x5c, 0x24, 8, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18},
                           reinterpret_cast<Address>(reward_callback));
        crystal_hook.attach(
            0x76ebe0, {0x48, 0x89, 0x5c, 0x24, 8, 0x57, 0x48, 0x83, 0xec, 0x20, 0x0f, 0xb6, 0xda, 0x48, 0x8b, 0xf9},
            reinterpret_cast<Address>(crystal_callback));
        relic_hook.attach(
            0x73c780, {0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xec, 0x30, 0x45, 0x8b, 0xf8, 0x4c, 0x8b, 0xe2},
            reinterpret_cast<Address>(relic_callback));
        time_trial_hook.attach(0x73cdf0,
                               {0x48, 0x89, 0x5c, 0x24, 8, 0x4c, 0x89, 0x4c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18},
                               reinterpret_cast<Address>(time_trial_callback));
        relic_count_hook.attach(0x75d150,
                                {0x48, 0x89, 0x5c, 0x24, 8, 0x57, 0x48, 0x83, 0xec, 0x20, 0x8b, 0xfa, 0x8b, 0xd9},
                                reinterpret_cast<Address>(relic_count_callback));
        collectible_query_hook.attach(
            0x75baf0, {0x48, 0x89, 0x5c, 0x24, 8, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18},
            reinterpret_cast<Address>(has_rewarded_callback));
        // award only into the active campaign
        if (zone.size() < 7 || zone.rfind("crash", 0) != 0 || zone[5] < '1' || zone[5] > '3' || !player())
            return;
        const auto campaign = static_cast<unsigned>(zone[5] - '0');
        const auto world = value<Address>(base() + 0x1a64690);
        if (!world || reinterpret_cast<unsigned (*)(Address)>(base() + 0x131400)(world) != campaign)
            return;
        const auto save = reinterpret_cast<Address (*)()>(base() + 0x7dee10)();
        if (!save || !typed(value<Address>(save + 0x458), 0x140c530))
            return;
        const auto descriptor = value<Address>(base() + 0x1a58348);
        // descriptor offsets identify the session row's relic rank (+0x48), crystal flag (+0x4c) and reward mask (+0x50)
        if (!descriptor || value<std::uint16_t>(descriptor + 24) != 0x48 ||
            value<std::uint16_t>(value<Address>(base() + 0x1a58398) + 24) != 0x50 ||
            value<std::uint16_t>(value<Address>(base() + 0x1a58368) + 24) != 0x4c)
            return;
        const auto zones = list(value<Address>(value<Address>(base() + 0x1a5b690) + 40), 4096);
        for (auto zone_address : zones)
        {
            if (!typed(zone_address, 0x13d0fb0) || value<unsigned>(zone_address + 120) != campaign + 6)
                continue;
            const auto level = level_key(string(value<Address>(zone_address + 16)));
            for (unsigned i = 0; i < state.collectible_count; ++i)
            {
                const auto &c = state.collectibles[i];
                if (level != c.key)
                    continue;
                Address wrapper = 0, row = 0;
                if (!zone_session(zone_address, wrapper, row))
                    continue;
                {
                    std::lock_guard lock(collectible_mutex);
                    const auto spec = collectible_spec(c.key);
                    auto observed = collectible_observed(value<unsigned>(row + 0x50));
                    if (value<unsigned>(row + 0x48))
                        observed |= 0x2000;
                    if (value<unsigned char>(row + 0x4c))
                        observed |= 0x200;
                    collectible_earned[level] |= collectible_pickups_from_save(
                        observed & collectible_mask(*spec), c.selected, c.owned);
                }
                if (!c.owned)
                    continue;
                state.collectible_awards +=
                    grant_collectibles(zone_address, wrapper, row, c.owned, descriptor,
                                       reinterpret_cast<void (*)(Address, Address, unsigned, int)>(base() + 0x73c780),
                                       reinterpret_cast<void (*)(unsigned, int)>(base() + 0x75d150));
            }
        }
    }

} // namespace ingame
