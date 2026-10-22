#include "game_memory.h"

#include <psapi.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <set>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace
{
    // Vtable/function constants are executable RVAs. graph offsets are relative to each loaded script slab.
    struct GemRouteSpec
    {
        const char *name;
        unsigned meta, data_meta, fields, data_fields, null_test, test, property, accessor, field, data_accessor, yes, no;
        std::uintptr_t yes_type, no_type;
    };
    constexpr std::array<GemRouteSpec, 3> gem_routes{{
#include "gem_route_specs.inc"
    }};
    constexpr std::array<const char *, 6> gem_colors{{"Blue", "Green", "Orange", "Purple", "Red", "Yellow"}};
    // engine resource-name hashes identify gem handles. these are not Archipelago item IDs.
    constexpr std::array<unsigned, 6> gem_hashes{{0x0c570659, 0x153bf938, 0xfda979ef, 0x6caaeedb, 0x2f5911e8, 0x0198d765}};
    constexpr std::array<const char *, 6> upgrade_names{{"Upgrade_SpeedShoesCrash2", "Upgrade_SuperChargedBodySlam",
                                                         "Upgrade_DoubleJump", "Upgrade_DeathTornadoSpin", "Upgrade_FruitBazooka", "Upgrade_SpeedShoes"}};
    constexpr std::array<unsigned, 6> upgrade_hashes{{0xb4db53b2, 0x4c074554, 0xad92650d, 0x9a2eea5f, 0x30c8272d, 0x4b50a5f9}};
    struct AbilityQuery
    {
        unsigned item, test, property, accessor, field, yes, no;
        const char *field_name;
    };
    constexpr std::array<AbilityQuery, 5> ability_queries{{
        {0, 86700, 59316, 59364, 59404, 72132, 72420, "Game_Bool_Variable_id_24zld8g1_metaField"},
        {1, 72908, 60340, 60388, 60428, 72948, 73212, "Game_Bool_Variable_id_ky5bqwyu_metaField"},
        {3, 71356, 59828, 59876, 59916, 71396, 71660, "Game_Bool_Variable_id_c5t0joe9_metaField"},
        {4, 70540, 59572, 59620, 59660, 70580, 70868, "Game_Bool_Variable_id_v7e7hd5w"},
        {5, 72092, 60084, 60132, 60172, 72132, 72420, "Game_Bool_Variable_id_b35hkok7_metaField"},
    }};
    constexpr std::array<unsigned char, 5> original_jump_call{{0xe8, 0xf1, 0x60, 0x03, 0x00}};
}

namespace
{
    constexpr std::uintptr_t portal_vtable = 0x14c8ef8;
    constexpr std::uintptr_t component_vtable = 0x148dd58;
    constexpr std::uintptr_t data_vtable = 0x148dc98;
    struct PortalSpec
    {
        const char *level;
        std::array<unsigned char, 12> position;
        std::uint32_t bottom_z_bits = 0;
        std::uint32_t radius_bits = 0x42f00000;
    };
    constexpr std::array<PortalSpec, 35> portals{{
#include "c2_portal_specs.inc"
    }};
    constexpr std::array<PortalSpec, 5> lifts{{
#include "c2_lift_specs.inc"
    }};
    struct C3ButtonSpec
    {
        const char *key;
        const char *name;
        std::array<unsigned char, 12> position;
        unsigned asset_offset;
    };
    constexpr std::array<C3ButtonSpec, 36> c3_buttons{{
#include "c3_button_specs.inc"
    }};
    constexpr std::array<C3ButtonSpec, 34> c1_spots{{
#include "c1_map_specs.inc"
    }};
    struct C3BarrierSpec
    {
        const char *name;
        std::array<unsigned char, 12> position;
        unsigned asset_offset;
    };
    constexpr std::array<C3BarrierSpec, 4> c3_barriers{{
#include "c3_barrier_specs.inc"
    }};
    constexpr std::array<C3BarrierSpec, 7> c3_portals{{
#include "c3_portal_specs.inc"
    }};
    struct C3PlatformSpec
    {
        const char *name;
        std::array<unsigned char, 12> position;
        unsigned asset_offset, data_offset;
        bool enabled;
    };
    constexpr std::array<C3PlatformSpec, 3> c3_platforms{{
#include "c3_secret_platform_specs.inc"
    }};
    constexpr std::array<unsigned char, 32> sphere_fields{{3, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
                                                           0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x70, 0x42, 0, 0, 0, 0}};
    // C2 trigger fields start at data+56. fields_for replaces enabled (+12), bottom Z (+32) and radius (+48)
    constexpr std::array<unsigned char, 52> trigger_fields = {
        0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x96, 0x43,
        0x00, 0x00, 0xf0, 0x42};

    template <typename T>
    T value(const unsigned char *raw, std::size_t offset)
    {
        T result;
        std::memcpy(&result, raw + offset, sizeof(result));
        return result;
    }

    void pin_component_data_copy(unsigned char *data)
    {
        // preserve the object's engine flags at +8 while replacing its reference-count bits for the private copy
        const unsigned flags = (value<unsigned>(data, 8) & 0xffc80000u) | 0x40000u;
        std::memcpy(data + 8, &flags, 4);
    }

    template <std::size_t N>
    std::array<unsigned char, N> read(HANDLE process, std::uintptr_t address)
    {
        std::array<unsigned char, N> raw{};
        SIZE_T count = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void *>(address), raw.data(), N, &count) || count != N)
            throw std::runtime_error("Cannot read verified game object");
        return raw;
    }

    bool readable(const MEMORY_BASIC_INFORMATION &region)
    {
        const DWORD protection = region.Protect & 0xff;
        return region.State == MEM_COMMIT && !(region.Protect & PAGE_GUARD) &&
               (protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY);
    }

    std::size_t next_entity_offset(const unsigned char *bytes, std::size_t count, std::size_t offset,
                                   std::uintptr_t vtable)
    {
        for (; offset <= count && count - offset >= sizeof(vtable); offset += sizeof(vtable))
            if (value<std::uintptr_t>(bytes, offset) == vtable)
                return offset;
        return count;
    }

    std::uintptr_t module_base(HANDLE process)
    {
        HMODULE module = nullptr;
        DWORD needed = 0;
        MODULEINFO info{};
        if (!EnumProcessModules(process, &module, sizeof(module), &needed) || needed < sizeof(module) ||
            !GetModuleInformation(process, module, &info, sizeof(info)) || info.lpBaseOfDll != module ||
            portal_vtable >= info.SizeOfImage)
            throw std::runtime_error("Cannot verify NST module");
        return reinterpret_cast<std::uintptr_t>(module);
    }

    const PortalSpec &spec_for(const std::string &level)
    {
        const auto it = std::find_if(portals.begin(), portals.end(),
                                     [&level](const auto &spec)
                                     { return level == spec.level; });
        if (it == portals.end())
            throw std::invalid_argument("Unknown Crash 2 portal");
        return *it;
    }

    std::array<unsigned char, 52> fields_for(const std::string &level, bool enabled)
    {
        auto fields = trigger_fields;
        const auto bottom = spec_for(level).bottom_z_bits;
        std::memcpy(fields.data() + 32, &bottom, sizeof(bottom));
        const auto radius = spec_for(level).radius_bits;
        std::memcpy(fields.data() + 48, &radius, sizeof(radius));
        fields[12] = enabled ? 1 : 0;
        return fields;
    }

    std::size_t match_portal(const unsigned char *raw, std::uintptr_t base,
                             const std::string &level, unsigned floor)
    {
        if (value<std::uintptr_t>(raw, 0) != base + portal_vtable || !value<std::uintptr_t>(raw, 64))
            return portals.size();
        for (std::size_t i = 0; i < portals.size(); ++i)
        {
            const auto room = i < 25 ? i / 5 + 1 : i < 30 ? i - 24
                                                          : 6;
            if ((!level.empty() && level != portals[i].level) || (floor && room != floor))
                continue;
            if (std::memcmp(raw + 32, portals[i].position.data(), 12) == 0)
                return i;
        }
        return portals.size();
    }

    std::string metadata_name(HANDLE process, std::uintptr_t address)
    {
        const auto raw = read<128>(process, address);
        const auto end = std::find(raw.begin(), raw.end(), 0);
        if (end == raw.end())
            throw std::runtime_error("Unterminated boss metadata name");
        return std::string(raw.begin(), end);
    }

    struct MetadataRegistry
    {
        std::uintptr_t list = 0, data = 0;
        unsigned count = 0;
        std::array<unsigned char, 24> header{};
    };
    MetadataRegistry metadata_registry(HANDLE process, std::uintptr_t context)
    {
        if (!context)
            return {};
        const auto list = value<std::uintptr_t>(read<40>(process, context).data(), 32);
        if (!list)
            return {};
        const auto header = read<24>(process, list);
        // registry header: count +0, packed byte length +8, element pointer +16. high size bits hold engine flags.
        const auto count = value<unsigned>(header.data(), 0);
        const auto bytes = value<std::uint64_t>(header.data(), 8) & 0xfffffffffffffULL;
        const auto data = value<std::uintptr_t>(header.data(), 16);
        if (count > 65535 || bytes > 65536 * 8 || bytes < count * 8ULL || (count && !data))
            throw std::runtime_error("Invalid engine metadata registry bounds");
        return {list, data, count, header};
    }
    std::map<std::string, std::uintptr_t> script_registry(HANDLE process, std::uintptr_t base, std::uintptr_t context)
    {
        std::map<std::string, std::uintptr_t> result;
        const auto registry = metadata_registry(process, context);
        const auto [list, data, count, header] = registry;
        if (!list)
            return result;
        std::vector<std::uintptr_t> entries(count);
        SIZE_T copied = 0;
        if (count && (!ReadProcessMemory(process, reinterpret_cast<void *>(data), entries.data(), count * 8, &copied) || copied != count * 8))
            throw std::runtime_error("Cannot read engine metadata registry");
        for (const auto meta : entries)
        {
            if (!meta)
                continue;
            const auto object = read<24>(process, meta);
            if (value<std::uintptr_t>(object.data(), 0) != base + 0x1745d78)
                continue;
            const auto name = metadata_name(process, value<std::uintptr_t>(object.data(), 16));
            if (name.rfind("Scripts.Graph.", 0) != 0)
                continue;
            const auto [it, inserted] = result.emplace(name, meta);
            if (!inserted && it->second != meta)
                throw std::runtime_error("Ambiguous engine script metadata: " + name);
        }
        if (read<24>(process, list) != header || value<std::uintptr_t>(read<40>(process, context).data(), 32) != list)
            throw std::runtime_error("Engine metadata registry changed during discovery");
        return result;
    }

    std::uintptr_t crate_metadata_pointer(HANDLE process, std::uintptr_t base, std::uintptr_t context, CrateMetadataPointer &cached)
    {
        constexpr const char *name = "Scripts.Graph.common_Crate_LevelCount";
        const auto registry = metadata_registry(process, context);
        if (!registry.list)
        {
            cached = {};
            return 0;
        }
        if (cached.meta && cached.list == registry.list && cached.header == registry.header &&
            cached.slot >= registry.data && (cached.slot - registry.data) % 8 == 0 && (cached.slot - registry.data) / 8 < registry.count)
        {
            try
            {
                const auto meta = value<std::uintptr_t>(read<8>(process, cached.slot).data(), 0);
                const auto object = read<24>(process, meta);
                if (meta == cached.meta && value<std::uintptr_t>(object.data(), 0) == base + 0x1745d78 &&
                    metadata_name(process, value<std::uintptr_t>(object.data(), 16)) == name &&
                    read<24>(process, registry.list) == registry.header)
                {
                    return meta;
                }
            }
            catch (const std::exception &)
            {
            } // a retired slot must go through discovery again
        }
        cached = {};
        const auto scripts = script_registry(process, base, context);
        const auto found = scripts.find(name);
        if (found == scripts.end())
            return 0;
        std::vector<std::uintptr_t> entries(registry.count);
        SIZE_T copied = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void *>(registry.data), entries.data(), entries.size() * 8, &copied) || copied != entries.size() * 8)
            throw std::runtime_error("Cannot read crate metadata pointer table");
        const auto slot = std::find(entries.begin(), entries.end(), found->second);
        if (slot == entries.end() || read<24>(process, registry.list) != registry.header)
            throw std::runtime_error("Engine metadata registry changed during crate discovery");
        cached.list = registry.list;
        cached.header = registry.header;
        cached.meta = found->second;
        cached.slot = registry.data + static_cast<std::uintptr_t>(slot - entries.begin()) * 8;
        return cached.meta;
    }

    std::uintptr_t boss_field(HANDLE process, std::uintptr_t meta, std::uintptr_t owner,
                              const char *name, unsigned expected_offset, unsigned expected_size,
                              std::uintptr_t expected_vtable, std::uint32_t properties)
    {
        // field metadata: size at +22, owner-relative offset at +24, properties at +40 and name pointer at +48
        const auto raw = read<48>(process, meta);
        const auto count = value<std::uint32_t>(raw.data(), 24);
        if (count > 256 || value<std::uint32_t>(raw.data(), 32) < count * 8)
            throw std::runtime_error("Invalid boss metadata field list");
        std::uintptr_t result = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto field = read<72>(process, value<std::uintptr_t>(
                                                     read<8>(process, value<std::uintptr_t>(raw.data(), 40) + i * 8).data(), 0));
            if (metadata_name(process, value<std::uintptr_t>(field.data(), 48)) != name)
                continue;
            if (result || value<std::uintptr_t>(field.data(), 0) != expected_vtable ||
                value<std::uint16_t>(field.data(), 22) != expected_size ||
                value<std::uint16_t>(field.data(), 24) != expected_offset ||
                value<std::uint32_t>(field.data(), 40) != properties)
                throw std::runtime_error("Boss field differs from verified runtime metadata");
            result = owner + expected_offset;
        }
        if (!result)
            throw std::runtime_error("Verified boss field is missing");
        return result;
    }

    void probe_boss_script(HANDLE process, std::uintptr_t base, std::uintptr_t list,
                           unsigned count, const std::string &level, C2PortalProbe &result)
    {
        bool found_entry = false;
        for (unsigned i = 0; i < count; ++i)
        {
            const auto address = value<std::uintptr_t>(read<8>(process, list + i * 8).data(), 0);
            const auto component = read<64>(process, address);
            if (value<std::uintptr_t>(component.data(), 0) != base + 0x13bbbd0 ||
                value<std::uintptr_t>(component.data(), 24) != result.entity)
                continue;
            const auto meta_address = value<std::uintptr_t>(component.data(), 56);
            const auto meta = read<184>(process, meta_address);
            const auto name = metadata_name(process, value<std::uintptr_t>(meta.data(), 16));
            const bool boss = name == "Scripts.Graph.common_C2_Hub_BossPortal";
            if (!boss && name != "Scripts.Graph.common_C2_WarpRoom_LevelPortal")
                continue;
            if ((boss ? result.boss_component != 0 : found_entry) ||
                value<std::uint16_t>(meta.data(), 90) != (boss ? 156 : 836) ||
                value<std::uint32_t>(meta.data(), 24) != (boss ? 35u : 129u) ||
                value<std::uint16_t>(meta.data(), 96) || value<std::uintptr_t>(component.data(), 48))
                throw std::runtime_error("Boss portal script layout differs from verified metadata");
            const auto data_address = value<std::uintptr_t>(component.data(), 16);
            const auto data = read<40>(process, data_address);
            const auto dm_address = value<std::uintptr_t>(data.data(), 32);
            if (value<std::uintptr_t>(data.data(), 0) != base + 0x13bbb10 ||
                dm_address != value<std::uintptr_t>(meta.data(), 176))
                throw std::runtime_error("Boss portal behavior/data metadata mismatch");
            const auto dm = read<104>(process, dm_address);
            if (metadata_name(process, value<std::uintptr_t>(dm.data(), 16)) != name + "Data" ||
                value<std::uint16_t>(dm.data(), 90) != (boss ? 57 : 616) ||
                value<std::uint32_t>(dm.data(), 24) != (boss ? 8u : 76u))
                throw std::runtime_error("Boss portal data layout differs from verified metadata");
            if (boss)
            {
                result.boss_component = address;
                result.boss_data = data_address;
                const auto threshold = boss_field(process, dm_address, data_address,
                                                  "Int_id_6a1x1hjz", 40, 4, base + 0x17a0ec0, 0xc413ff);
                if (value<std::int32_t>(read<4>(process, threshold).data(), 0) != (level[3] - '0') * 5)
                    throw std::runtime_error("Boss crystal requirement differs from installed asset");
            }
            else
            {
                found_entry = true;
                result.entry_component = address;
                result.entry_meta = meta_address;
                const auto allowed = boss_field(process, meta_address, address,
                                                "Bool_id_imgtocxq", 392, 1, base + 0x17a1858, 0xc407ff);
                result.entry_allowed = read<1>(process, allowed)[0];
                const auto gate_field = boss_field(process, meta_address, address,
                                                   "InternalStore__gate_id_1pu7p5di", 104, 8, base + 0x17a4c20, 0xc023ff);
                result.entry_gate = value<std::uintptr_t>(read<8>(process, gate_field).data(), 0);
                const auto gate = read<24>(process, result.entry_gate);
                if (value<std::uintptr_t>(gate.data(), 0) != base + 0x1759da0)
                    throw std::runtime_error("Boss portal entry gate type differs");
                const auto gate_count = value<std::int32_t>(gate.data(), 16);
                if (result.entry_allowed > 1 || gate[20] > 1 || gate_count < 0 || gate_count > 64)
                    throw std::runtime_error("Invalid boss portal entry state");
                for (unsigned j = 0; j < 2; ++j)
                {
                    const auto field = boss_field(process, meta_address, address,
                                                  j ? "Vfx_Spawned_Effect_id_aoca5amg" : "vfxTeleporter_id_lozbtd2z",
                                                  j ? 424 : 96, 8, base + 0x17a46b8, 0xc423ff);
                    const auto handle = value<std::uintptr_t>(read<8>(process, field).data(), 0);
                    if (!handle)
                        continue;
                    const auto reference = read<48>(process, handle);
                    // read only already resolved handles
                    if (value<std::uint32_t>(reference.data(), 0) & 0x30000000)
                        throw std::runtime_error("Boss effect handle is not resolved");
                    result.effects[j] = value<std::uintptr_t>(reference.data(), 40);
                }
            }
        }
        if (!result.boss_component || !found_entry)
            throw std::runtime_error("Verified boss portal scripts are missing");
    }
}

C2BossEventProbe probe_c2_boss_events_at(HANDLE process, std::uintptr_t entity, const std::string &level)
{
    if (level.size() != 4 || level.substr(0, 3) != "B20" || level[3] < '1' || level[3] > '5')
        throw std::invalid_argument("Expected Crash 2 boss B201..B205");
    C2BossEventProbe result{};
    result.portal = probe_c2_portal_at(process, entity, level);
    if (!result.portal.hub_active)
        return result;
    const auto base = module_base(process);
    // IGZ objects are fixed in place
    const auto slab = result.portal.entry_meta - 144092;
    const auto node = [&](unsigned offset, std::uintptr_t vtable)
    {
        const auto address = slab + offset;
        if (value<std::uintptr_t>(read<8>(process, address).data(), 0) != base + vtable)
            throw std::runtime_error("Boss event graph differs from verified runtime layout");
        return address;
    };
    node(144092, 0x1745d78); // common_C2_WarpRoom_LevelPortal metadata
    const auto entry = node(155804, 0x1746848);
    result.entry_output = entry + 48;
    result.original_output = slab + 155996; // helperObject001: original OnCEnter gate check
    if (value<std::uintptr_t>(read<8>(process, result.entry_output).data(), 0) != result.original_output)
        throw std::runtime_error("Portal entry graph already has an override");
    result.owner_accessor = node(126708, 0x13bbcb8);
    const auto copy = [&](unsigned source, std::uintptr_t vtable, unsigned destination, unsigned length)
    {
        const auto address = node(source, vtable);
        SIZE_T count = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void *>(address),
                               result.templates.data() + destination, length, &count) ||
            count != length)
            throw std::runtime_error("Cannot read boss event template");
    };
    copy(160044, 0x1759220, 0, 48);  // object comparison
    copy(118948, 0x1747690, 64, 32); // constant object accessor
    const auto handle = value<std::uintptr_t>(result.templates.data(), 64 + 24);
    const auto reference = read<48>(process, handle);
    if (value<std::uint32_t>(reference.data(), 0) & 0x30000000)
        throw std::runtime_error("Boss constant handle requires engine resolution");
    std::copy(reference.begin(), reference.end(), result.templates.begin() + 96);
    copy(154068, 0x13bc390, 160, 80); // enable: 3edj7qcd
    copy(213452, 0x13bc390, 320, 80); // open the entry gate: h5oxmnqv
    for (const auto offset : {160u, 320u})
        if (value<std::uintptr_t>(result.templates.data(), offset + 24) != result.owner_accessor)
            throw std::runtime_error("Boss message recipient differs from verified owner accessor");
    return result;
}

std::array<unsigned char, 512> c2_boss_event_copy(const C2BossEventProbe &probe, std::uintptr_t remote,
                                                  std::uintptr_t fallback)
{
    if (!remote || !probe.portal.entity || !probe.owner_accessor || !probe.original_output)
        throw std::invalid_argument("Invalid boss event copy addresses");
    auto copy = probe.templates;
    // slot: owner comparison +0, constant/handle +64/+96, enable event +160, open-gate event +320
    const auto pointer = [&](unsigned offset, std::uintptr_t value)
    {
        std::memcpy(copy.data() + offset, &value, sizeof(value));
    };
    pointer(16, probe.owner_accessor);
    pointer(24, remote + 64);
    pointer(32, probe.portal.entry_allowed ? probe.original_output : remote + 160);
    pointer(40, fallback ? fallback : probe.original_output);
    pointer(64 + 24, remote + 96);
    pointer(96 + 40, probe.portal.entity);
    // event next links are at +72. opening the gate finishes this private chain.
    pointer(160 + 72, remote + 320);
    pointer(320 + 72, 0);
    return copy;
}

std::vector<unsigned char> c2_boss_event_chain(const std::vector<C2BossEventProbe> &probes, std::uintptr_t remote)
{
    if (probes.empty() || probes.size() > 5 || !remote ||
        remote > std::numeric_limits<std::uintptr_t>::max() - probes.size() * 512)
        throw std::invalid_argument("Invalid boss event chain size/address");
    std::vector<unsigned char> result(probes.size() * 512);
    for (std::size_t i = 0; i < probes.size(); ++i)
    {
        const auto &probe = probes[i];
        if (!probe.entry_output || probe.entry_output != probes[0].entry_output ||
            probe.original_output != probes[0].original_output || probe.owner_accessor != probes[0].owner_accessor)
            throw std::invalid_argument("Boss portals do not share the verified entry graph");
        for (std::size_t j = 0; j < i; ++j)
            if (probe.portal.entity == probes[j].portal.entity)
                throw std::invalid_argument("Duplicate boss entity in event chain");
        const auto copy = c2_boss_event_copy(probe, remote + i * 512,
                                             i + 1 < probes.size() ? remote + (i + 1) * 512 : probe.original_output);
        std::copy(copy.begin(), copy.end(), result.begin() + i * 512);
    }
    return result;
}

C2PortalProbe probe_c2_portal_at(HANDLE process, std::uintptr_t entity,
                                 const std::string &level, std::uintptr_t cloned_data)
{
    const auto base = module_base(process);
    const auto &position = spec_for(level).position;
    const auto portal = read<72>(process, entity);
    if (value<std::uintptr_t>(portal.data(), 0) != base + portal_vtable ||
        !std::equal(position.begin(), position.end(), portal.begin() + 32) ||
        !value<std::uintptr_t>(portal.data(), 64))
        throw std::runtime_error("Cached Crash 2 portal entity changed: " + level);
    C2PortalProbe result{};
    result.entity = entity;
    result.hub_active = (value<std::uint32_t>(portal.data(), 56) & 0x100) != 0;
    if (!result.hub_active)
        return result;

    const auto list = value<std::uintptr_t>(portal.data(), 24);
    const auto header = read<40>(process, list);
    const auto count = value<std::uint32_t>(header.data(), 16);
    const auto capacity = value<std::uint32_t>(header.data(), 20);
    if (count == 0 || count > capacity || capacity > 16)
        throw std::runtime_error("Unexpected active portal component list");
    const auto array = value<std::uintptr_t>(header.data(), 32);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto address = value<std::uintptr_t>(read<8>(process, array + i * 8).data(), 0);
        const auto component = read<40>(process, address);
        if (value<std::uintptr_t>(component.data(), 0) != base + component_vtable ||
            value<std::uintptr_t>(component.data(), 24) != entity)
            continue;
        if (result.component)
            throw std::runtime_error("Multiple Crash 2 portal trigger components");
        result.component = address;
        result.data = value<std::uintptr_t>(component.data(), 16);
    }
    if (!result.component)
        throw std::runtime_error("Crash 2 portal trigger component missing");
    if (!verify_c2_trigger_data(process, result.data, level, result.data != cloned_data))
        throw std::runtime_error("Crash 2 portal trigger data differs from verified asset");
    if (level[0] == 'B')
        probe_boss_script(process, base, array, count, level, result);
    return result;
}

bool verify_c2_trigger_data(HANDLE process, std::uintptr_t address,
                            const std::string &level, bool events_enabled)
{
    try
    {
        const auto data = read<112>(process, address);
        const auto fields = fields_for(level, events_enabled);
        return value<std::uintptr_t>(data.data(), 0) == module_base(process) + data_vtable &&
               std::equal(fields.begin(), fields.end(), data.begin() + 56);
    }
    catch (const std::runtime_error &)
    {
        return false;
    }
}

namespace
{
    bool hub_entity_owners(HANDLE process, std::uintptr_t base, std::uintptr_t context,
                           std::uintptr_t events_meta, std::vector<std::uintptr_t> &owners)
    {
        const auto pointer = [&](std::uintptr_t at)
        { return value<std::uintptr_t>(read<8>(process, at).data(), 0); };
        owners.clear();
        if (!context || !events_meta)
            return false;
        const auto meta = read<64>(process, events_meta);
        if (metadata_name(process, value<std::uintptr_t>(meta.data(), 16)) != "CEntityEvents" ||
            value<std::uintptr_t>(meta.data(), 56) != base + 0x14cb600)
            throw std::runtime_error("Hub entity-event metadata changed");
        const auto table = pointer(context + 120);
        if (!table)
            return false;
        const auto header = read<56>(process, table);
        constexpr auto byte_mask = (1ULL << 52) - 1;
        const auto bytes = value<std::uint64_t>(header.data(), 16) & byte_mask;
        const auto key_bytes = value<std::uint64_t>(header.data(), 32) & byte_mask;
        const auto lists = value<std::uintptr_t>(header.data(), 24), keys = value<std::uintptr_t>(header.data(), 40);
        if (bytes != key_bytes || bytes % 8 || bytes > 65536 * 8 || (bytes && (!lists || !keys)))
            throw std::runtime_error("Invalid hub entity registry bounds");
        const auto pointers = [&](std::uintptr_t at, std::size_t count)
        {
            std::vector<std::uintptr_t> result(count);
            SIZE_T copied = 0;
            if (count && (!ReadProcessMemory(process, reinterpret_cast<void *>(at), result.data(), count * 8, &copied) || copied != count * 8))
                throw std::runtime_error("Cannot read hub registry entries");
            return result;
        };
        const auto key_entries = pointers(keys, bytes / 8), list_entries = pointers(lists, bytes / 8);
        std::uintptr_t events = 0;
        for (std::size_t i = 0; i < key_entries.size(); ++i)
        {
            if (key_entries[i] != events_meta)
                continue;
            if (events && events != list_entries[i])
                throw std::runtime_error("Ambiguous hub event lists");
            events = list_entries[i];
        }
        if (!events)
            return false;
        const auto anchors = read<40>(process, events);
        const auto count = value<unsigned>(anchors.data(), 16), capacity = value<unsigned>(anchors.data(), 20);
        const auto entries = value<std::uintptr_t>(anchors.data(), 32);
        if (count > 16384 || count > capacity || (count && !entries))
            throw std::runtime_error("Invalid hub event instance bounds");
        for (const auto event : pointers(entries, count))
        {
            if (!event)
                continue;
            const auto raw = read<32>(process, event);
            if (value<std::uintptr_t>(raw.data(), 0) != base + 0x14cb600)
                continue;
            const auto owner = value<std::uintptr_t>(raw.data(), 24);
            if (owner && pointer(owner) == base + portal_vtable)
                owners.push_back(owner);
        }
        if (read<56>(process, table) != header || pointer(context + 120) != table || read<40>(process, events) != anchors)
            throw std::runtime_error("Hub registry changed during discovery");
        std::sort(owners.begin(), owners.end());
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
        return true;
    }

    std::map<std::string, std::uintptr_t> scan_portals(DWORD pid, const std::string &level,
                                                       unsigned floor = 0, bool include_lifts = false,
                                                       C3ButtonDiscovery *c3 = nullptr)
    {
        if (!level.empty())
            spec_for(level);
        static_assert(sizeof(void *) == 8, "NST requires a 64-bit client");
        const auto close = [](HANDLE process)
        { if (process) CloseHandle(process); };
        std::unique_ptr<void, decltype(close)> process(
            OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid), close);
        if (!process)
            throw std::runtime_error("Cannot open NST process for reading");

        const auto image_base = module_base(process.get());

        const auto vtable = image_base + portal_vtable;
        std::array<std::vector<std::uintptr_t>, portals.size() + lifts.size()> matches;
        std::map<std::string, std::vector<std::uintptr_t>> c3_matches;
        const auto match = [&](std::uintptr_t found, const unsigned char *raw)
        {
            if (value<std::uintptr_t>(raw, 0) != vtable)
                return;
            const auto i = match_portal(raw, image_base, level, floor);
            if (i != portals.size())
                matches[i].push_back(found);
            else if (include_lifts && value<std::uintptr_t>(raw, 64))
            {
                for (std::size_t j = 0; j < lifts.size(); ++j)
                    if (std::memcmp(raw + 32, lifts[j].position.data(), 12) == 0)
                        matches[portals.size() + j].push_back(found);
            }
            if (c3 && value<std::uintptr_t>(raw, 64))
            {
                const auto name_pointer = value<std::uintptr_t>(read<8>(process.get(), found + 128).data(), 0);
                const auto name = metadata_name(process.get(), name_pointer);
                for (const auto &button : c3_buttons)
                {
                    if (name == button.name)
                        c3_matches[button.key].push_back(found);
                }
            }
        };
        const auto context = value<std::uintptr_t>(read<8>(process.get(), image_base + 0x1ac1dd8).data(), 0);
        const auto events_meta = value<std::uintptr_t>(read<8>(process.get(), image_base + 0x1a744a0).data(), 0);
        std::vector<std::uintptr_t> owners;
        const bool registry = hub_entity_owners(process.get(), image_base, context, events_meta, owners);
        if (registry)
        {
            for (const auto owner : owners)
                match(owner, read<72>(process.get(), owner).data());
            if (value<std::uintptr_t>(read<8>(process.get(), image_base + 0x1ac1dd8).data(), 0) != context)
                throw std::runtime_error("Hub entity context changed during discovery");
        }
        std::vector<unsigned char> buffer(1 << 20);
        for (std::uintptr_t address = 0; !registry && address < 0x7fff00000000ULL;)
        {
            MEMORY_BASIC_INFORMATION region{};
            if (!VirtualQueryEx(process.get(), reinterpret_cast<void *>(address), &region, sizeof(region)))
                break;
            const auto region_base = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
            const auto end = region_base + region.RegionSize;
            if (end <= address)
                break;
            if (readable(region))
            {
                for (auto pos = region_base; pos < end;)
                {
                    const auto length = std::min<std::size_t>(buffer.size(), end - pos);
                    SIZE_T count = 0;
                    if (ReadProcessMemory(process.get(), reinterpret_cast<void *>(pos), buffer.data(), length, &count))
                    {
                        auto offset = static_cast<std::size_t>((8 - (pos & 7)) & 7);
                        while ((offset = next_entity_offset(buffer.data(), count, offset, vtable)) != count)
                        {
                            const auto found = pos + offset;
                            try
                            {
                                std::array<unsigned char, 72> raw{};
                                if (offset + raw.size() <= count)
                                    std::copy_n(buffer.begin() + offset, raw.size(), raw.begin());
                                else
                                    raw = read<72>(process.get(), found);
                                match(found, raw.data());
                            }
                            catch (const std::runtime_error &)
                            {
                            }
                            offset += 8;
                        }
                    }
                    if (pos + length == end)
                        break;
                    pos += length - std::min<std::size_t>(length - 1, 71);
                }
            }
            address = end;
        }
        std::map<std::string, std::uintptr_t> result;
        if (c3)
        {
            for (auto &[key, found] : c3_matches)
            {
                std::sort(found.begin(), found.end());
                found.erase(std::unique(found.begin(), found.end()), found.end());
                if (found.size() != 1)
                    throw std::runtime_error(key + ": ambiguous Crash 3 button owners");
                c3->entities.emplace(key, found.front());
            }
        }
        for (std::size_t i = 0; i < portals.size() + (include_lifts ? lifts.size() : 0); ++i)
        {
            const auto &spec = i < portals.size() ? portals[i] : lifts[i - portals.size()];
            const auto room = i < 25 ? i / 5 + 1 : i < 30           ? i - 24
                                               : i < portals.size() ? 6
                                                                    : i - portals.size() + 1;
            if ((!level.empty() && level != spec.level) || (floor && room != floor))
                continue;
            auto &found = matches[i];
            std::sort(found.begin(), found.end());
            found.erase(std::unique(found.begin(), found.end()), found.end());
            if (found.size() > 1)
                throw std::runtime_error(std::string(spec.level) + ": ambiguous live hub entities");
            if (!found.empty())
                result.emplace(spec.level, found.front());
        }
        return result;
    }

}

namespace
{
    const C3ButtonSpec &c3_spec(const std::string &key)
    {
        const auto found = std::find_if(c3_buttons.begin(), c3_buttons.end(),
                                        [&](const auto &spec)
                                        { return key == spec.key; });
        if (found == c3_buttons.end())
            throw std::invalid_argument("Unknown Crash 3 button");
        return *found;
    }

    bool c3_identity_matches(const C3ButtonSpec &spec, const unsigned char *raw,
                             const std::string &name, std::uintptr_t base)
    {
        if (std::string(spec.name).rfind("C3_Hub_Portal_Button", 0) != 0 &&
            std::memcmp(raw + 32, spec.position.data(), 12) != 0)
            return false;
        for (unsigned axis = 0; axis < 3; ++axis)
        {
            const auto current = value<float>(raw, 32 + axis * 4), original = value<float>(spec.position.data(), axis * 4);
            if (!std::isfinite(current) || std::abs(current - original) > 1024)
                return false;
        }
        return value<std::uintptr_t>(raw, 0) == base + portal_vtable &&
               value<std::uintptr_t>(raw, 64) && name == spec.name;
    }

    std::uintptr_t c3_type_vtable(HANDLE process, std::uintptr_t meta, const char *name, unsigned size = 0)
    {
        const auto raw = read<104>(process, meta);
        const auto base = module_base(process);
        const auto vtable = value<std::uintptr_t>(raw.data(), 56);
        const auto actual_size = value<std::uint16_t>(raw.data(), 90);
        MODULEINFO image{};
        if (!GetModuleInformation(process, reinterpret_cast<HMODULE>(base), &image, sizeof(image)))
            throw std::runtime_error("Cannot verify Crash 3 type module bounds");
        if (metadata_name(process, value<std::uintptr_t>(raw.data(), 16)) != name ||
            actual_size < 40 || actual_size > 4096 || (size && actual_size != size) ||
            value<std::uint16_t>(raw.data(), 96) || vtable < base || vtable >= base + image.SizeOfImage ||
            value<std::uintptr_t>(raw.data(), 0) < base || value<std::uintptr_t>(raw.data(), 0) >= base + image.SizeOfImage)
            throw std::runtime_error("Crash 3 engine type metadata differs from the expected layout");
        return vtable;
    }

    std::uintptr_t c3_script_parent(HANDLE process, std::uintptr_t entity)
    {
        const auto raw = read<72>(process, entity);
        if (!(value<std::uint32_t>(raw.data(), 56) & 0x100))
            return 0;
        const auto list = read<40>(process, value<std::uintptr_t>(raw.data(), 24));
        const auto count = value<std::uint32_t>(list.data(), 16);
        if (!count || count > value<std::uint32_t>(list.data(), 20) || count > 32)
            throw std::runtime_error("Unexpected Crash 3 button component list");
        for (unsigned i = 0; i < count; ++i)
        {
            const auto ptr = value<std::uintptr_t>(read<8>(process, value<std::uintptr_t>(list.data(), 32) + 8 * i).data(), 0);
            const auto component = read<64>(process, ptr);
            if (value<std::uintptr_t>(component.data(), 0) != module_base(process) + 0x13bbbd0 ||
                value<std::uintptr_t>(component.data(), 24) != entity)
                continue;
            const auto meta = read<184>(process, value<std::uintptr_t>(component.data(), 56));
            if (value<std::uintptr_t>(meta.data(), 0) != module_base(process) + 0x1745d78 ||
                metadata_name(process, value<std::uintptr_t>(meta.data(), 16)) !=
                    "Scripts.Graph.common_C3_Hub_Portal_Button")
                continue;
            const auto data = read<40>(process, value<std::uintptr_t>(component.data(), 16));
            if (value<std::uintptr_t>(data.data(), 0) != module_base(process) + 0x13bbb10 ||
                value<std::uintptr_t>(data.data(), 32) != value<std::uintptr_t>(meta.data(), 176))
                throw std::runtime_error("Crash 3 button behavior/data mismatch");
            return value<std::uintptr_t>(meta.data(), 64);
        }
        throw std::runtime_error("Crash 3 button script is missing");
    }

    bool c3_sphere_fields_match(const unsigned char *raw, bool allow_disabled = false)
    {
        auto fields = sphere_fields;
        if (allow_disabled && raw[68] <= 1)
            fields[12] = raw[68];
        return std::all_of(raw + 24, raw + 48, [](auto byte)
                           { return byte == 0; }) &&
               std::equal(fields.begin(), fields.end(), raw + 56);
    }

    std::array<std::uintptr_t, 2> c3_sphere_vtables(HANDLE process, std::uintptr_t entity)
    {
        const auto raw = read<72>(process, entity);
        const auto list = read<40>(process, value<std::uintptr_t>(raw.data(), 24));
        const auto count = value<std::uint32_t>(list.data(), 16);
        if (!count || count > value<std::uint32_t>(list.data(), 20) || count > 32)
            throw std::runtime_error("Unexpected Crash 3 discovery component list");
        std::array<std::uintptr_t, 2> result{};
        for (unsigned i = 0; i < count; ++i)
        {
            try
            {
                const auto ptr = value<std::uintptr_t>(read<8>(process, value<std::uintptr_t>(list.data(), 32) + 8 * i).data(), 0);
                const auto component = read<40>(process, ptr);
                if (value<std::uintptr_t>(component.data(), 24) != entity)
                    continue;
                const auto data = read<88>(process, value<std::uintptr_t>(component.data(), 16));
                if (!c3_sphere_fields_match(data.data(), true))
                    continue;
                if (result[0])
                    throw std::logic_error("Ambiguous Crash 3 discovery sphere layouts");
                result = {value<std::uintptr_t>(component.data(), 0), value<std::uintptr_t>(data.data(), 0)};
            }
            catch (const std::runtime_error &)
            {
            }
        }
        if (!result[0] || !result[1] || result[0] == result[1])
            throw std::runtime_error("No unique sphere layout on the active Crash 3 button");
        // here: read-only candidate. named engine metadata must verify both classes before attachment
        return result;
    }

    std::uintptr_t c3_script_meta_address(std::uintptr_t instance_field)
    {
        if (instance_field < 56 || (instance_field & 7))
            throw std::invalid_argument("Invalid engine metadata instance field address");
        return instance_field - 56; // igMetaObject._vTablePointer, independent of the metadata object's class.
    }

    C3TriggerTypes c3_sphere_metadata(HANDLE process, std::uintptr_t component_global, std::uintptr_t data_global)
    {
        C3TriggerTypes result{value<std::uintptr_t>(read<8>(process, component_global).data(), 0),
                              value<std::uintptr_t>(read<8>(process, data_global).data(), 0)};
        if (bool(result.component_meta) != bool(result.data_meta))
            throw std::runtime_error("Crash 3 sphere metadata is only partially initialized");
        return result;
    }
}

C3ButtonDiscovery find_c3_buttons(DWORD pid)
{
    C3ButtonDiscovery found;
    scan_portals(pid, "", 0, false, &found);
    const auto close = [](HANDLE handle)
    { if (handle) CloseHandle(handle); };
    std::unique_ptr<void, decltype(close)> process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid), close);
    if (!process)
        throw std::runtime_error("Cannot open NST for Crash 3 discovery");
    std::uintptr_t parent = 0;
    std::array<std::uintptr_t, 2> instance_vtables{};
    for (const auto &[key, entity] : found.entities)
    {
        parent = c3_script_parent(process.get(), entity);
        if (parent)
        {
            instance_vtables = c3_sphere_vtables(process.get(), entity);
            break;
        }
    }
    if (!parent)
    {
        found.entities.clear();
        return found;
    }
    if (c3_type_vtable(process.get(), parent, "CVscComponent") != module_base(process.get()) + 0x13bbbd0)
        throw std::runtime_error("Crash 3 script parent type differs");
    const auto base = module_base(process.get());
    found.types = c3_sphere_metadata(process.get(), base + 0x1a6e2e0, base + 0x1a6de68);
    if (found.types.component_meta)
    {
        if (c3_type_vtable(process.get(), found.types.component_meta, "CTriggerVolumeSphereComponent") != instance_vtables[0] ||
            c3_type_vtable(process.get(), found.types.data_meta, "CTriggerVolumeSphereComponentData", 88) != instance_vtables[1])
            throw std::runtime_error("Crash 3 sphere globals differ from the live button types");
        const auto current = c3_sphere_metadata(process.get(), base + 0x1a6e2e0, base + 0x1a6de68);
        if (current.component_meta != found.types.component_meta || current.data_meta != found.types.data_meta)
            throw std::runtime_error("Crash 3 sphere metadata changed during discovery");
        return found;
    }
    MEMORY_BASIC_INFORMATION region{};
    if (!VirtualQueryEx(process.get(), reinterpret_cast<void *>(parent), &region, sizeof(region)) || !readable(region))
        throw std::runtime_error("Cannot read engine type metadata region");
    // script/engine metadata have different object vtables. match their instance-vtable field instead
    std::vector<unsigned char> buffer(1 << 20);
    const auto end = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
    for (auto pos = reinterpret_cast<std::uintptr_t>(region.BaseAddress); pos < end;)
    {
        const auto length = std::min<std::size_t>(buffer.size(), end - pos);
        SIZE_T count = 0;
        if (ReadProcessMemory(process.get(), reinterpret_cast<void *>(pos), buffer.data(), length, &count))
        {
            for (unsigned kind = 0; kind < instance_vtables.size(); ++kind)
            {
                for (auto offset = static_cast<std::size_t>((8 - (pos & 7)) & 7);
                     (offset = next_entity_offset(buffer.data(), count, offset, instance_vtables[kind])) != count; offset += 8)
                {
                    try
                    {
                        const auto address = c3_script_meta_address(pos + offset);
                        const auto meta = read<104>(process.get(), address);
                        const auto name = metadata_name(process.get(), value<std::uintptr_t>(meta.data(), 16));
                        if (name != (kind == 0 ? "CTriggerVolumeSphereComponent" : "CTriggerVolumeSphereComponentData"))
                            continue;
                        auto *destination = kind == 0 ? &found.types.component_meta : &found.types.data_meta;
                        if (c3_type_vtable(process.get(), address, name.c_str(), kind == 1 ? 88 : 0) != instance_vtables[kind])
                            continue;
                        if (*destination && *destination != address)
                            throw std::logic_error("Ambiguous Crash 3 engine type metadata");
                        *destination = address;
                    }
                    catch (const std::runtime_error &)
                    {
                    }
                }
            }
        }
        if (pos + length == end)
            break;
        pos += length - std::min<std::size_t>(length - 1, 103);
    }
    if (!found.types.component_meta || !found.types.data_meta)
        throw std::runtime_error("Crash 3 sphere type metadata not found. No gates attached");
    return found;
}

bool verify_c3_trigger_data(HANDLE process, std::uintptr_t address, const C3TriggerTypes &types, bool enabled)
{
    try
    {
        const auto raw = read<88>(process, address);
        auto fields = sphere_fields;
        fields[12] = enabled ? 1 : 0;
        return value<std::uintptr_t>(raw.data(), 0) ==
                   c3_type_vtable(process, types.data_meta, "CTriggerVolumeSphereComponentData", 88) &&
               std::all_of(raw.begin() + 24, raw.begin() + 48, [](auto byte)
                           { return byte == 0; }) &&
               std::equal(fields.begin(), fields.end(), raw.begin() + 56);
    }
    catch (const std::runtime_error &)
    {
        return false;
    }
}

C2PortalProbe probe_c3_button_at(HANDLE process, std::uintptr_t entity, const std::string &key,
                                 const C3TriggerTypes &types, std::uintptr_t clone)
{
    const auto &spec = c3_spec(key);
    const auto raw = read<144>(process, entity);
    if (!c3_identity_matches(spec, raw.data(), metadata_name(process, value<std::uintptr_t>(raw.data(), 128)),
                             module_base(process)))
        throw std::runtime_error("Cached Crash 3 button identity changed: " + key);
    C2PortalProbe result;
    result.entity = entity;
    result.hub_active = (value<std::uint32_t>(raw.data(), 56) & 0x100) != 0;
    if (!result.hub_active)
        return result;
    if (!c3_script_parent(process, entity))
        throw std::runtime_error("Crash 3 hub changed during probe");
    const auto header = read<40>(process, value<std::uintptr_t>(raw.data(), 24));
    const auto count = value<std::uint32_t>(header.data(), 16);
    if (!count || count > value<std::uint32_t>(header.data(), 20) || count > 32)
        throw std::runtime_error("Unexpected Crash 3 trigger list");
    const auto vtable = c3_type_vtable(process, types.component_meta, "CTriggerVolumeSphereComponent");
    for (unsigned i = 0; i < count; ++i)
    {
        const auto ptr = value<std::uintptr_t>(read<8>(process, value<std::uintptr_t>(header.data(), 32) + 8 * i).data(), 0);
        const auto component = read<40>(process, ptr);
        if (value<std::uintptr_t>(component.data(), 0) != vtable ||
            value<std::uintptr_t>(component.data(), 24) != entity)
            continue;
        if (result.component)
            throw std::runtime_error("Ambiguous Crash 3 sphere trigger");
        result.component = ptr;
        result.data = value<std::uintptr_t>(component.data(), 16);
    }
    if (!result.component || !verify_c3_trigger_data(process, result.data, types, result.data != clone))
        throw std::runtime_error("Crash 3 sphere trigger differs from installed assets");
    return result;
}

std::vector<unsigned char> trigger_data_copy(std::vector<unsigned char> data)
{
    // both supported trigger layouts keep the event-enable byte at +68
    if ((data.size() != 88 && data.size() != 112) || data[68] != 1)
        throw std::invalid_argument("NST source trigger has an unsupported size or is not enabled");
    pin_component_data_copy(data.data());
    data[68] = 0;
    return data;
}

std::map<std::string, std::uintptr_t> find_c2_hub_entities(DWORD pid) { return scan_portals(pid, "", 0, true); }

C2LiftProbe probe_c2_lift_at(HANDLE process, std::uintptr_t entity, unsigned floor)
{
    if (floor < 1 || floor > 5)
        throw std::invalid_argument("Lift floor must be 1..5");
    const auto base = module_base(process);
    const auto text_at = [process](std::uintptr_t address)
    {
        const auto raw = read<96>(process, address);
        const auto end = std::find(raw.begin(), raw.end(), 0);
        if (end == raw.end())
            throw std::runtime_error("Unterminated lift metadata name");
        return std::string(raw.begin(), end);
    };
    const auto raw = read<144>(process, entity);
    if (value<std::uintptr_t>(raw.data(), 0) != base + portal_vtable ||
        !value<std::uintptr_t>(raw.data(), 64) ||
        text_at(value<std::uintptr_t>(raw.data(), 128)) != lifts[floor - 1].level)
        throw std::runtime_error("Cached Crash 2 lift entity changed");
    C2LiftProbe result{};
    result.entity = entity;
    result.hub_active = (value<std::uint32_t>(raw.data(), 56) & 0x100) != 0;
    if (!result.hub_active)
        return result;
    const auto header = read<40>(process, value<std::uintptr_t>(raw.data(), 24));
    const auto count = value<std::uint32_t>(header.data(), 16);
    const auto capacity = value<std::uint32_t>(header.data(), 20);
    if (!count || count > capacity || capacity > 16)
        throw std::runtime_error("Unexpected lift component list");
    for (std::uint32_t i = 0; i < count; ++i)
    {
        const auto address = value<std::uintptr_t>(
            read<8>(process, value<std::uintptr_t>(header.data(), 32) + i * 8).data(), 0);
        const auto component = read<64>(process, address);
        if (value<std::uintptr_t>(component.data(), 0) != base + 0x13bbbd0 ||
            value<std::uintptr_t>(component.data(), 24) != entity)
            continue;
        const auto meta = read<200>(process, value<std::uintptr_t>(component.data(), 56));
        if (text_at(value<std::uintptr_t>(meta.data(), 16)) != "Scripts.Graph.common_Crash2_Hub_WarpLift")
            continue;
        if (result.component || value<std::uint16_t>(meta.data(), 90) != 236 ||
            value<std::uint16_t>(meta.data(), 96) || value<std::uintptr_t>(component.data(), 48) ||
            value<std::uintptr_t>(meta.data(), 56) != base + 0x13bbbd0 ||
            value<std::uint32_t>(meta.data(), 24) != 48 || value<std::uint32_t>(meta.data(), 32) < 48 * 8)
            throw std::runtime_error("Lift script layout differs from verified runtime metadata");
        result.data = value<std::uintptr_t>(component.data(), 16);
        const auto data = read<40>(process, result.data);
        if (value<std::uintptr_t>(data.data(), 32) != value<std::uintptr_t>(meta.data(), 176))
            throw std::runtime_error("Lift behavior/data metadata mismatch");
        const auto data_meta = read<104>(process, value<std::uintptr_t>(data.data(), 32));
        if (value<std::uintptr_t>(data.data(), 0) != base + 0x13bbb10 ||
            text_at(value<std::uintptr_t>(data_meta.data(), 16)) != "Scripts.Graph.common_Crash2_Hub_WarpLiftData" ||
            value<std::uint16_t>(data_meta.data(), 90) != 232 ||
            value<std::uint32_t>(data_meta.data(), 24) != 31 ||
            value<std::uint32_t>(data_meta.data(), 32) < 31 * 8)
            throw std::runtime_error("Lift data layout differs from verified runtime metadata");
        constexpr std::array<const char *, 3> dependency_names = {
            "Game_Bool_Variable_id_l75gyu4w", "Zone_Info_id_d524ft42", "Zone_Info_id_c7k58up5_metaField"};
        constexpr std::array<unsigned, 3> dependency_offsets = {120, 168, 176};
        std::array<bool, 3> dependencies_found{};
        for (unsigned j = 0; j < 31; ++j)
        {
            const auto field = read<72>(process, value<std::uintptr_t>(
                                                     read<8>(process, value<std::uintptr_t>(data_meta.data(), 40) + j * 8).data(), 0));
            const auto name = text_at(value<std::uintptr_t>(field.data(), 48));
            for (unsigned k = 0; k < dependency_names.size(); ++k)
            {
                if (name != dependency_names[k])
                    continue;
                if (dependencies_found[k] || value<std::uintptr_t>(field.data(), 0) != base + 0x17a46b8 ||
                    value<std::uint16_t>(field.data(), 22) != 8 ||
                    value<std::uint16_t>(field.data(), 24) != dependency_offsets[k] ||
                    value<std::uint32_t>(field.data(), 40) != 0xc423ff)
                    throw std::runtime_error("Lift requirement field differs from verified metadata");
                dependencies_found[k] = true;
            }
        }
        if (std::find(dependencies_found.begin(), dependencies_found.end(), false) != dependencies_found.end())
            throw std::runtime_error("Verified lift requirement fields are missing");
        result.component = address;
        constexpr std::array<const char *, 4> names = {
            "Bool_id_0e6xjtcs", "Bool_id_vu1zwpxt", "Bool_id_mdjrgb72", "Bool_id_pvpu5h6a"};
        constexpr std::array<unsigned, 4> offsets = {161, 162, 224, 225};
        // first two fields are live lift permissions, the last two are its corresponding script input values.
        std::array<bool, 4> found{};
        for (unsigned j = 0; j < 48; ++j)
        {
            const auto field = read<72>(process, value<std::uintptr_t>(
                                                     read<8>(process, value<std::uintptr_t>(meta.data(), 40) + j * 8).data(), 0));
            const auto name = text_at(value<std::uintptr_t>(field.data(), 48));
            for (unsigned k = 0; k < names.size(); ++k)
            {
                if (name != names[k])
                    continue;
                if (found[k] || value<std::uintptr_t>(field.data(), 0) != base + 0x17a1858 ||
                    value<std::uint16_t>(field.data(), 22) != 1 ||
                    value<std::uint16_t>(field.data(), 24) != offsets[k] ||
                    value<std::uint32_t>(field.data(), 40) != 0xc407ff)
                    throw std::runtime_error("Lift permission field differs from verified metadata");
                const auto permission = address + offsets[k];
                const auto enabled = read<1>(process, permission)[0];
                if (enabled > 1)
                    throw std::runtime_error("Invalid lift permission value");
                if (k < 2)
                {
                    result.permissions[k] = permission;
                    result.values[k] = enabled;
                }
                else
                    result.input_values[k - 2] = enabled;
                found[k] = true;
            }
        }
        if (std::find(found.begin(), found.end(), false) != found.end())
            throw std::runtime_error("Verified lift permission/input fields are missing");
    }
    if (!result.component || !result.permissions[0] || !result.permissions[1])
        throw std::runtime_error("Verified lift permission fields are missing");
    return result;
}

std::vector<std::string> c2_portal_levels()
{
    std::vector<std::string> result;
    for (const auto &portal : portals)
        if (portal.level[0] == 'L' || portal.level[0] == 'S')
            result.emplace_back(portal.level);
    return result;
}

std::array<unsigned char, 232> c2_lift_data_override(std::array<unsigned char, 232> data, unsigned floor)
{
    if (floor < 1 || floor > 5)
        throw std::invalid_argument("Lift floor must be 1..5");
    if (c2_lift_direction_allowed(floor, 0))
        std::fill_n(data.begin() + 168, 8, 0);
    if (c2_lift_direction_allowed(floor, 1))
        std::fill_n(data.begin() + 120, 8, 0);
    return data;
}

namespace
{
    std::array<std::uintptr_t, 3> c3_behavior(HANDLE process, std::uintptr_t entity, const char *name,
                                              unsigned size, unsigned fields, unsigned data_size, unsigned data_fields)
    {
        const auto base = module_base(process);
        const auto list = read<40>(process, value<std::uintptr_t>(read<32>(process, entity).data(), 24));
        const auto count = value<unsigned>(list.data(), 16);
        if (!count || count > 32 || count > value<unsigned>(list.data(), 20))
            throw std::runtime_error("Invalid C3 access component list");
        std::array<std::uintptr_t, 3> result{};
        for (unsigned i = 0; i < count; ++i)
        {
            const auto ptr = value<std::uintptr_t>(read<8>(process, value<std::uintptr_t>(list.data(), 32) + 8 * i).data(), 0);
            const auto component = read<64>(process, ptr);
            if (value<std::uintptr_t>(component.data(), 0) != base + 0x13bbbd0 ||
                value<std::uintptr_t>(component.data(), 24) != entity)
                continue;
            const auto meta_ptr = value<std::uintptr_t>(component.data(), 56);
            const auto meta = read<184>(process, meta_ptr);
            if (metadata_name(process, value<std::uintptr_t>(meta.data(), 16)) != name)
                continue;
            const auto data_ptr = value<std::uintptr_t>(component.data(), 16);
            const auto data = read<40>(process, data_ptr);
            const auto dm_ptr = value<std::uintptr_t>(meta.data(), 176);
            const auto dm = read<104>(process, dm_ptr);
            if (result[0] || value<std::uintptr_t>(meta.data(), 0) != base + 0x1745d78 ||
                (size ? value<std::uint16_t>(meta.data(), 90) != size : (value<std::uint16_t>(meta.data(), 90) < 64 || value<std::uint16_t>(meta.data(), 90) > 4096)) ||
                value<unsigned>(meta.data(), 24) != fields ||
                value<std::uint16_t>(meta.data(), 96) || value<std::uintptr_t>(component.data(), 48) ||
                value<std::uintptr_t>(data.data(), 0) != base + 0x13bbb10 ||
                value<std::uintptr_t>(data.data(), 32) != dm_ptr ||
                metadata_name(process, value<std::uintptr_t>(dm.data(), 16)) != std::string(name) + "Data" ||
                (data_size ? value<std::uint16_t>(dm.data(), 90) != data_size : (value<std::uint16_t>(dm.data(), 90) < 40 || value<std::uint16_t>(dm.data(), 90) > 2048)) ||
                value<unsigned>(dm.data(), 24) != data_fields ||
                value<std::uint16_t>(dm.data(), 96))
                throw std::runtime_error("C3 access script/data differs from verified live metadata");
            result = {ptr, data_ptr, meta_ptr};
        }
        if (!result[0])
            throw std::runtime_error("C3 access behavior is missing");
        return result;
    }

    std::uintptr_t c3_graph_node(HANDLE process, std::uintptr_t slab, unsigned offset, std::uintptr_t rva)
    {
        // slab offsets locate serialized nodes. the executable-relative vtable verifies each node's type.
        const auto address = slab + offset;
        if (value<std::uintptr_t>(read<8>(process, address).data(), 0) != module_base(process) + rva)
            throw std::runtime_error("C3 access graph node differs from verified runtime type");
        return address;
    }
}

C3ButtonEventProbe probe_c3_button_events_at(HANDLE process, std::uintptr_t entity, const std::string &key,
                                             std::uintptr_t override_route, std::uintptr_t contact_override)
{
    const auto &spec = c3_spec(key);
    const auto raw = read<144>(process, entity);
    if (!c3_identity_matches(spec, raw.data(), metadata_name(process, value<std::uintptr_t>(raw.data(), 128)), module_base(process)) ||
        !(value<unsigned>(raw.data(), 56) & 0x100))
        throw std::runtime_error("C3 button inactive or changed. Engine route is not writable");
    const auto behavior = c3_behavior(process, entity, "Scripts.Graph.common_C3_Hub_Portal_Button", 144, 29, 161, 23);
    C3ButtonEventProbe result;
    result.entity = entity;
    result.script = behavior[0];
    result.meta = behavior[2];
    const auto base = module_base(process);
    const auto enabled = read<1>(process, boss_field(process, result.meta, result.script,
                                                 "Bool_id_1kmcqa3h", 128, 1, base + 0x17a1858, 0xc407ff))[0];
    const auto busy = read<1>(process, boss_field(process, result.meta, result.script,
                                              "Bool_id_pekgy1w1", 88, 1, base + 0x17a1858, 0xc407ff))[0];
    if (enabled > 1 || busy > 1)
        throw std::runtime_error("Invalid C3 activation state");
    const auto slab = result.meta - 42340;
    // rebase the compiled portal-button graph from its metadata anchor. each node is type-checked below.
    const auto node = [&](unsigned offset, std::uintptr_t rva)
    { return c3_graph_node(process, slab, offset, rva); };
    node(42340, 0x1745d78);
    result.entry_output = node(51900, 0x1746848) + 48;
    result.original_output = node(52052, 0x1759168);
    const auto root = value<std::uintptr_t>(read<8>(process, result.entry_output).data(), 0);
    if (root != result.original_output && (!override_route || root != override_route))
        throw std::runtime_error("C3 button activation graph has an unknown override");
    result.visibility_outputs = {node(56556, 0x1746848) + 48, node(55644, 0x1746848) + 48};
    result.original_visibility = {node(56708, 0x175dbd8), node(55796, 0x175dbd8)};
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto route = value<std::uintptr_t>(read<8>(process, result.visibility_outputs[i]).data(), 0);
        if (route != result.original_visibility[i] && (!override_route || route != override_route))
            throw std::runtime_error("C3 button visibility graph has an unknown override");
    }
    result.contact_output = node(49516, 0x1746848) + 48;
    result.original_contact = node(49676, 0x1745c08);
    const auto contact_root = value<std::uintptr_t>(read<8>(process, result.contact_output).data(), 0);
    if (contact_root != result.original_contact && (!override_route || contact_root != (contact_override ? contact_override : override_route + 320)))
        throw std::runtime_error("C3 button contact graph has an unknown override");
    result.busy_check = node(52132, 0x1759168);
    result.owner_accessor = node(31516, 0x13bbcb8);
    const auto enabled_accessor = node(35260, 0x17483c0);
    if (value<std::uintptr_t>(read<32>(process, enabled_accessor).data(), 24) != slab + 35292 ||
        value<std::uintptr_t>(read<40>(process, result.busy_check).data(), 24))
        throw std::runtime_error("C3 enabled/busy accessor chain changed");
    node(35292, 0x17a1858);
    const auto busy_accessor = node(35708, 0x17483c0);
    const auto busy_field = node(35740, 0x17a1858);
    if (value<std::uintptr_t>(read<24>(process, result.busy_check).data(), 16) != busy_accessor ||
        value<std::uintptr_t>(read<32>(process, busy_accessor).data(), 24) != busy_field ||
        value<std::uint16_t>(read<26>(process, busy_field).data(), 24) != 88)
        throw std::runtime_error("C3 busy check differs from the game activation guard");
    const auto copy = [&](unsigned offset, std::uintptr_t rva, unsigned dest, unsigned length)
    {
        SIZE_T count = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void *>(node(offset, rva)),
                               result.templates.data() + dest, length, &count) ||
            count != length)
            throw std::runtime_error("Cannot read C3 activation template");
    };
    copy(39996, 0x1759220, 0, 48);
    copy(31292, 0x1747690, 64, 32);
    const auto handle = value<std::uintptr_t>(result.templates.data(), 88);
    const auto reference = read<48>(process, handle);
    if (value<unsigned>(reference.data(), 0) & 0x30000000)
        throw std::runtime_error("C3 constant handle requires engine resolution");
    std::copy(reference.begin(), reference.end(), result.templates.begin() + 96);
    copy(56708, 0x175dbd8, 160, 40); // engine enable assignment, continued directly to the original busy check
    const auto constant = node(56748, 0x1748280);
    if (value<std::uintptr_t>(result.templates.data(), 176) != constant || read<1>(process, constant + 24)[0] != 1 ||
        value<std::uintptr_t>(result.templates.data(), 184) != enabled_accessor)
        throw std::runtime_error("C3 engine enable assignment changed");
    copy(56780, 0x13bc390, 240, 80); // engine enable's activation message. the contact dispatcher sends it for owned buttons
    if (value<std::uintptr_t>(result.templates.data(), 264) != result.owner_accessor ||
        value<std::uintptr_t>(result.templates.data(), 256) != slab + 44084)
        throw std::runtime_error("C3 game activation message recipient/type changed");
    copy(55796, 0x175dbd8, 384, 40);
    copy(55868, 0x13bc390, 424, 80); // engine Force_Hide disables the trigger and hides its model
    const auto false_constant = node(55836, 0x1748280);
    if (value<std::uintptr_t>(result.templates.data(), 400) != false_constant || read<1>(process, false_constant + 24)[0] ||
        value<std::uintptr_t>(result.templates.data(), 408) != enabled_accessor ||
        value<std::uintptr_t>(result.templates.data(), 416) != slab + 55868 ||
        value<std::uintptr_t>(result.templates.data(), 440) != node(55948, 0x17b6578) ||
        value<std::uintptr_t>(result.templates.data(), 448) != result.owner_accessor ||
        value<std::uintptr_t>(result.templates.data(), 496))
        throw std::runtime_error("C3 engine hide assignment/message changed");
    return result;
}

std::array<C3BarrierEventProbe, 4> probe_c3_barriers_at(HANDLE process, std::uintptr_t button,
                                                        const std::string &key, std::uintptr_t override_route)
{
    const auto &spec = c3_spec(key);
    const auto button_raw = read<144>(process, button);
    const auto base = module_base(process);
    if (!c3_identity_matches(spec, button_raw.data(), metadata_name(process, value<std::uintptr_t>(button_raw.data(), 128)), base) ||
        !(value<unsigned>(button_raw.data(), 56) & 0x100))
        throw std::runtime_error("C3 traversal anchor is inactive or changed");
    const auto hub = button - spec.asset_offset;
    std::array<C3BarrierEventProbe, 4> result{};
    for (unsigned i = 0; i < result.size(); ++i)
    {
        const auto &gate = c3_barriers[i];
        auto &probe = result[i];
        probe.entity = hub + gate.asset_offset;
        const auto raw = read<144>(process, probe.entity);
        // verified CPhysicalEntity instance vtable in the pinned EXE
        if (value<std::uintptr_t>(raw.data(), 0) != base + 0x141d490 ||
            metadata_name(process, value<std::uintptr_t>(raw.data(), 128)) != gate.name ||
            !std::equal(gate.position.begin(), gate.position.end(), raw.begin() + 32))
            throw std::runtime_error("C3 barrier owner identity changed");
        const auto behavior = c3_behavior(process, probe.entity, "Scripts.Graph.L300_Hub_C3_Hub_ElectricGate", 176, 30, 152, 19);
        probe.meta = behavior[2];
        const auto slab = probe.meta - 23764;
        const auto node = [&](unsigned offset, std::uintptr_t rva)
        { return c3_graph_node(process, slab, offset, rva); };
        probe.entry_output = node(22092, 0x1746848) + 48;
        probe.original_output = node(22260, 0x175dbd8);
        probe.disable = node(20116, 0x175dbd8);
        const auto constant = node(20156, 0x1748280);
        probe.false_value = constant;
        const auto property = node(20188, 0x1748780);
        const auto owner = node(16548, 0x13bbcb8);
        probe.owner_accessor = owner;
        probe.enabled_property = read<48>(process, property);
        const auto assignment = read<40>(process, probe.disable);
        const auto root = value<std::uintptr_t>(read<8>(process, probe.entry_output).data(), 0);
        if ((root != probe.original_output && (!override_route || root != override_route)) ||
            value<std::uintptr_t>(assignment.data(), 16) != constant || read<1>(process, constant + 24)[0] ||
            value<std::uintptr_t>(assignment.data(), 24) != property || value<std::uintptr_t>(assignment.data(), 32) ||
            value<std::uintptr_t>(read<32>(process, property).data(), 24) != owner ||
            !value<std::uintptr_t>(probe.enabled_property.data(), 32) || !value<std::uintptr_t>(probe.enabled_property.data(), 40) ||
            (i && (probe.entry_output != result[0].entry_output || probe.disable != result[0].disable)))
            throw std::runtime_error("C3 engine barrier disable route changed");
    }
    return result;
}

std::vector<std::string> c3_button_keys()
{
    std::vector<std::string> result;
    for (const auto &spec : c3_buttons)
        result.emplace_back(spec.key);
    return result;
}
int c3_button_index(const std::string &name)
{
    for (unsigned i = 0; i < c3_buttons.size(); ++i)
        if (name == c3_buttons[i].name)
            return static_cast<int>(i);
    return -1;
}

C3PortalEntryProbe probe_c3_portal_entry_at(HANDLE process, std::uintptr_t button, const std::string &key,
                                            std::uintptr_t override_route)
{
    const auto &anchor = c3_spec(key);
    const auto base = module_base(process);
    const auto raw = read<144>(process, button);
    if (!c3_identity_matches(anchor, raw.data(), metadata_name(process, value<std::uintptr_t>(raw.data(), 128)), base) || button < anchor.asset_offset)
        throw std::runtime_error("C3 portal anchor identity changed");
    C3PortalEntryProbe result;
    if (!(value<unsigned>(raw.data(), 56) & 0x100))
        return result;
    result.hub_active = true;
    const auto hub = button - anchor.asset_offset;
    for (unsigned i = 0; i < c3_portals.size(); ++i)
    {
        const auto &spec = c3_portals[i];
        const auto entity = hub + spec.asset_offset;
        const auto owner = read<144>(process, entity);
        if (value<std::uintptr_t>(owner.data(), 0) != base + portal_vtable ||
            metadata_name(process, value<std::uintptr_t>(owner.data(), 128)) != spec.name ||
            !std::equal(spec.position.begin(), spec.position.end(), owner.begin() + 32))
            throw std::runtime_error("C3 portal template identity changed");
        const auto behavior = c3_behavior(process, entity, "Scripts.Graph.common_C3_Hub_LevelPortal", 1568, 243, 552, 69);
        if (i && result.meta != behavior[2])
            throw std::runtime_error("C3 portals do not share one entry graph");
        result.owners[i] = entity;
        result.meta = behavior[2];
        const auto slab = result.meta - 233020;
        result.entry_output = c3_graph_node(process, slab, 248692, 0x1746848) + 48;
        result.original_output = c3_graph_node(process, slab, 248884, 0x1759ef0);
        result.destination_accessor = c3_graph_node(process, slab, 197524, 0x17477d0);
        result.relic_output = c3_graph_node(process, slab, 280884, 0x1745c08) + 40;
        result.relic_original = c3_graph_node(process, slab, 280988, 0x1758f40);
        result.relic_unlocked = c3_graph_node(process, slab, 281068, 0x175dbd8);
        const auto relic_compare = read<80>(process, result.relic_original);
        if (value<std::uintptr_t>(relic_compare.data(), 16) != c3_graph_node(process, slab, 280956, 0x1754fe0) ||
            value<std::uintptr_t>(relic_compare.data(), 24) != c3_graph_node(process, slab, 279492, 0x1754fe0) ||
            value<std::uintptr_t>(relic_compare.data(), 40) != result.relic_unlocked ||
            value<std::uintptr_t>(relic_compare.data(), 72) != c3_graph_node(process, slab, 281140, 0x175dbd8))
            throw std::runtime_error("C3 portal relic comparison changed");
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto setter = read<32>(process, c3_graph_node(process, slab, i ? 281140 : 281068, 0x175dbd8));
            const auto constant = c3_graph_node(process, slab, i ? 281180 : 281108, 0x1748280);
            if (value<std::uintptr_t>(setter.data(), 16) != constant || read<1>(process, constant + 24)[0] != (i ? 0 : 1) ||
                value<std::uintptr_t>(setter.data(), 24) != c3_graph_node(process, slab, 212260, 0x17483c0))
                throw std::runtime_error("C3 portal relic result assignment changed");
        }
        const auto relic_route = value<std::uintptr_t>(read<8>(process, result.relic_output).data(), 0);
        if (relic_route != result.relic_original && (!override_route || relic_route != result.relic_unlocked))
            throw std::runtime_error("C3 portal relic query has an unknown override");
        if (value<std::uintptr_t>(read<24>(process, c3_graph_node(process, slab, 249180, 0x175dbd8)).data(), 16) != result.destination_accessor)
            throw std::runtime_error("C3 engine entry uses a different destination accessor");
        const auto field = read<72>(process, value<std::uintptr_t>(read<32>(process, result.destination_accessor).data(), 24));
        if (metadata_name(process, value<std::uintptr_t>(field.data(), 48)) != "Zone_Info_id_pgbq3mpt" ||
            value<std::uintptr_t>(field.data(), 0) != base + 0x17a46b8 ||
            value<std::uint16_t>(field.data(), 22) != 8 || value<std::uint16_t>(field.data(), 24) != 232 ||
            value<unsigned>(field.data(), 40) != 0xc423ff)
            throw std::runtime_error("C3 portal destination accessor differs from live metadata");
        boss_field(process, result.meta, behavior[0], "Zone_Info_id_pgbq3mpt", 232, 8, base + 0x17a46b8, 0xc423ff);
        const auto entry = value<std::uintptr_t>(read<8>(process, result.entry_output).data(), 0);
        if (entry != result.original_output && (!override_route || entry != override_route))
            throw std::runtime_error("C3 portal entry graph has an unknown override. Stop other portal tests first");
    }
    for (const auto &spec : c3_buttons)
    {
        const auto entity = hub + spec.asset_offset;
        const auto owner = read<144>(process, entity);
        if (!c3_identity_matches(spec, owner.data(), metadata_name(process, value<std::uintptr_t>(owner.data(), 128)), base) ||
            !(value<unsigned>(owner.data(), 56) & 0x100))
            throw std::runtime_error("C3 destination button is inactive or changed");
        const auto behavior = c3_behavior(process, entity, "Scripts.Graph.common_C3_Hub_Portal_Button", 144, 29, 161, 23);
        const auto data_meta = value<std::uintptr_t>(read<40>(process, behavior[1]).data(), 32);
        const auto address = boss_field(process, data_meta, behavior[1], "Zone_Info_id_0wfxuf0o", 64, 8, base + 0x17a46b8, 0xc423ff);
        C3PortalDestination destination;
        destination.key = std::string(spec.key).substr(0, 4);
        destination.handle = read<48>(process, value<std::uintptr_t>(read<8>(process, address).data(), 0));
        destination.zone = value<std::uintptr_t>(destination.handle.data(), 40);
        if ((value<unsigned>(destination.handle.data(), 0) & 0x30000000) || !destination.zone ||
            value<std::uintptr_t>(read<8>(process, destination.zone).data(), 0) != base + 0x13d0fb0)
            throw std::runtime_error("C3 destination handle is unresolved or has the wrong engine class");
        auto prefix = (destination.key[0] == 'B' ? "crash3/bosses/" : "crash3/") + destination.key + '_';
        for (auto &ch : prefix)
            if (ch >= 'A' && ch <= 'Z')
                ch += 'a' - 'A';
        const auto debug_name = metadata_name(process, value<std::uintptr_t>(read<40>(process, destination.zone).data(), 32));
        if (debug_name.rfind(prefix, 0) != 0)
            throw std::runtime_error("C3 " + destination.key + " destination handle belongs to " + debug_name);
        result.destinations.push_back(destination);
        if (result.destinations.size() == 1)
        {
            const auto slab = behavior[2] - 42340;
            result.comparison = read<48>(process, c3_graph_node(process, slab, 39996, 0x1759220));
            result.constant = read<32>(process, c3_graph_node(process, slab, 31292, 0x1747690));
        }
    }
    return result;
}

std::vector<unsigned char> c3_portal_entry_chain(const C3PortalEntryProbe &probe, std::uintptr_t remote)
{
    // each 256-byte slot holds an object comparison, a constant at +64 and its resolved zone handle at +96
    const auto count = probe.destinations.size();
    if (!count || count > 36 || !remote || (remote & 7) || !probe.destination_accessor || !probe.original_output ||
        remote > std::numeric_limits<std::uintptr_t>::max() - count * 256)
        throw std::invalid_argument("Invalid C3 portal entry chain size/address");
    std::vector<unsigned char> copied(count * 256);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &destination = probe.destinations[i];
        if (!destination.zone || (destination.zone & 7) ||
            value<std::uintptr_t>(destination.handle.data(), 40) != destination.zone ||
            (value<unsigned>(destination.handle.data(), 0) & 0x30000000))
            throw std::invalid_argument("Invalid resolved C3 destination");
        auto *raw = copied.data() + i * 256;
        const auto address = remote + i * 256;
        std::copy(probe.comparison.begin(), probe.comparison.end(), raw);
        std::copy(probe.constant.begin(), probe.constant.end(), raw + 64);
        std::copy(destination.handle.begin(), destination.handle.end(), raw + 96);
        const auto pointer = [&](unsigned offset, std::uintptr_t ptr)
        { std::memcpy(raw + offset, &ptr, 8); };
        pointer(16, probe.destination_accessor);
        pointer(24, address + 64);
        pointer(32, 0);                                 // denied until an access grant enables this destination
        pointer(40, i + 1 < count ? address + 256 : 0); // unknown/null destinations stop before engine teleport
        pointer(88, address + 96);
    }
    return copied;
}

namespace
{
    const C3ButtonSpec &c1_anchor_spec()
    {
        return *std::find_if(c1_spots.begin(), c1_spots.end(), [](const auto &s)
                             { return std::string(s.key) == "L101"; });
    }
    void c1_accessor_field(HANDLE process, std::uintptr_t meta, std::uintptr_t accessor,
                           const char *name, unsigned size, std::uintptr_t rva, unsigned properties)
    {
        const auto field_ptr = value<std::uintptr_t>(read<32>(process, accessor).data(), 24);
        const auto field = read<72>(process, field_ptr);
        const auto offset = value<std::uint16_t>(field.data(), 24);
        const auto compiled_size = value<std::uint16_t>(read<104>(process, meta).data(), 90);
        if (offset < 64 || offset > compiled_size || size > compiled_size - offset)
            throw std::runtime_error("C1 accessor field is outside compiled component");
        boss_field(process, meta, 0, name, offset, size, module_base(process) + rva, properties);
    }
}

C1MapProbe probe_c1_map_at(HANDLE process, const C1MapDiscovery &found,
                           std::uintptr_t entry_override, std::uintptr_t secret_override, bool traversal)
{
    if (!found.anchor || !found.manager_meta)
        throw std::runtime_error("Crash 1 map/manager graph is not loaded");
    const auto base = module_base(process);
    const auto &anchor = c1_anchor_spec();
    if (found.anchor < anchor.asset_offset || found.manager_meta < 142276)
        throw std::runtime_error("Invalid C1 map allocation");
    const auto hub = found.anchor - anchor.asset_offset;
    const auto meta = read<184>(process, found.manager_meta);
    if (value<std::uintptr_t>(meta.data(), 0) != base + 0x1745d78 ||
        metadata_name(process, value<std::uintptr_t>(meta.data(), 16)) != "Scripts.Graph.common_Crash1_Hub_Manager" ||
        value<unsigned>(meta.data(), 24) != 136 || value<std::uintptr_t>(meta.data(), 56) != base + 0x13bbbd0 ||
        value<std::uint16_t>(meta.data(), 90) < 64 || value<std::uint16_t>(meta.data(), 90) > 4096 ||
        value<std::uint16_t>(meta.data(), 96))
        throw std::runtime_error("C1 manager metadata differs from installed graph");
    const auto dm = read<104>(process, value<std::uintptr_t>(meta.data(), 176));
    if (metadata_name(process, value<std::uintptr_t>(dm.data(), 16)) != "Scripts.Graph.common_Crash1_Hub_ManagerData" ||
        value<unsigned>(dm.data(), 24) != 36 || value<std::uintptr_t>(dm.data(), 56) != base + 0x13bbb10 ||
        value<std::uint16_t>(dm.data(), 90) < 40 || value<std::uint16_t>(dm.data(), 90) > 2048 ||
        value<std::uint16_t>(dm.data(), 96))
        throw std::runtime_error("C1 manager data metadata changed");
    C1MapProbe result;
    result.discovery = found;
    auto &entry = result.entry;
    entry.meta = found.manager_meta;
    const auto slab = entry.meta - 142276;
    const auto node = [&](unsigned offset, std::uintptr_t rva)
    { return c3_graph_node(process, slab, offset, rva); };
    entry.entry_output = node(174644, 0x1746848) + 48;
    entry.original_output = node(174724, 0x13bc390);
    entry.destination_accessor = node(114796, 0x17477d0);
    if (value<std::uintptr_t>(read<32>(process, entry.original_output).data(), 24) != entry.destination_accessor)
        throw std::runtime_error("C1 load event no longer selects currMapSpot");
    c1_accessor_field(process, entry.meta, entry.destination_accessor, "currMapSpot", 8, 0x17a46b8, 0xc423ff);
    entry.comparison = read<48>(process, node(186412, 0x1759220));
    entry.constant = read<32>(process, node(111996, 0x1747690));
    const auto handle = read<48>(process, value<std::uintptr_t>(entry.constant.data(), 24));
    if (value<unsigned>(handle.data(), 0) & 0x30000000)
        throw std::runtime_error("C1 comparison constant is unresolved");
    result.loading_assignment = read<40>(process, node(140612, 0x175dbd8));
    const auto busy = node(132940, 0x17483c0);
    c1_accessor_field(process, entry.meta, busy, "Bool_id_cw893m2s", 1, 0x17a1858, 0xc407ff);
    if (value<std::uintptr_t>(result.loading_assignment.data(), 24) != busy ||
        value<std::uintptr_t>(result.loading_assignment.data(), 16) != node(134108, 0x1748280) ||
        read<1>(process, slab + 134108 + 24)[0] != 1)
        throw std::runtime_error("C1 loading latch assignment changed");
    result.false_value = node(134076, 0x1748280);
    result.resume_input = node(164396, 0x13bc390);
    const auto resume = read<80>(process, result.resume_input);
    if (read<1>(process, result.false_value + 24)[0] || value<std::uintptr_t>(resume.data(), 72) ||
        value<std::uintptr_t>(resume.data(), 16) != slab + 137156 ||
        value<std::uintptr_t>(resume.data(), 24) != node(111812, 0x13bbcb8))
        throw std::runtime_error("C1 denied-load recovery changed");
    const auto debug = node(158972, 0x1745c08);
    if (value<std::uintptr_t>(read<56>(process, debug).data(), 48) != slab + 159036)
        throw std::runtime_error("C1 navigation debug branch changed");
    const auto outputs = read<56>(process, slab + 159036);
    result.progression_route = value<std::uintptr_t>(outputs.data(), 32);
    result.original_progression = slab + 159092;
    result.bounds = slab + 159292;
    // the engine list-size check remains in control. both comparisons must retain their engine operands/routes.
    const auto clamp = read<72>(process, result.original_progression);
    const auto bounds = read<80>(process, result.bounds);
    const auto comparison_type = value<std::uintptr_t>(clamp.data(), 0);
    if (value<unsigned>(outputs.data(), 16) != 2 || value<unsigned>(outputs.data(), 20) < 2 ||
        result.progression_route != slab + 159036 + 40 ||
        value<std::uintptr_t>(clamp.data(), 16) != slab + 114628 ||
        value<std::uintptr_t>(clamp.data(), 24) != slab + 112340 ||
        value<std::uintptr_t>(clamp.data(), 64) != result.bounds ||
        value<std::uintptr_t>(bounds.data(), 16) != slab + 114628 ||
        value<std::uintptr_t>(bounds.data(), 72) != slab + 119636 ||
        comparison_type < base || comparison_type >= base + 0x2000000 ||
        comparison_type != value<std::uintptr_t>(bounds.data(), 0) ||
        value<std::uintptr_t>(outputs.data(), 48) != result.bounds)
        throw std::runtime_error("C1 navigation bounds changed");
    for (const auto &spec : c1_spots)
    {
        const auto entity = hub + spec.asset_offset;
        const auto owner = read<144>(process, entity);
        if (value<std::uintptr_t>(owner.data(), 0) != base + portal_vtable ||
            metadata_name(process, value<std::uintptr_t>(owner.data(), 128)) != spec.name ||
            !std::equal(spec.position.begin(), spec.position.end(), owner.begin() + 32))
            throw std::runtime_error("C1 map spot identity changed");
        entry.hub_active |= (value<unsigned>(owner.data(), 56) & 0x100) != 0;
        const auto behavior = c3_behavior(process, entity, "Scripts.Graph.common_Crash1_Hub_MapSpot", 0, 53, 0, 40);
        if (result.secrets.meta && result.secrets.meta != behavior[2])
            throw std::runtime_error("C1 map spots no longer share their engine graph");
        result.secrets.meta = behavior[2];
        C3PortalDestination destination;
        destination.key = spec.key;
        destination.zone = entity;
        destination.handle = handle;
        std::memcpy(destination.handle.data() + 40, &entity, 8);
        entry.destinations.push_back(destination);
        if (destination.key == "L114" || destination.key == "L123")
            result.secrets.destinations.push_back(destination);
    }
    auto &secrets = result.secrets;
    if (secrets.meta < 62812)
        throw std::runtime_error("Invalid C1 map-spot graph");
    const auto spot = secrets.meta - 62812;
    secrets.entry_output = c3_graph_node(process, spot, 71676, 0x1746848) + 48;
    secrets.original_output = c3_graph_node(process, spot, 71868, 0x1745c08);
    secrets.destination_accessor = c3_graph_node(process, spot, 57100, 0x13bbcb8);
    secrets.comparison = entry.comparison;
    secrets.constant = entry.constant;
    // only the two key-gated map paths get engine movement. other secret actors keep their original handler.
    secrets.owners[0] = c3_graph_node(process, spot, 72460, 0x13bc390);
    // shortcut movement asks its target for the vanilla unlock flag. 
    // AP map traversal must also allow returning travel
    const auto pending = c3_graph_node(process, spot, 104036, 0x1759168);
    const auto approval = c3_graph_node(process, spot, 104076, 0x1759168);
    result.shortcut_route = approval + 32;
    result.shortcut_denied = c3_graph_node(process, spot, 102260, 0x175dbd8);
    result.shortcut_move = c3_graph_node(process, spot, 104116, 0x13bc390);
    const auto pending_accessor = c3_graph_node(process, spot, 56220, 0x17483c0);
    const auto approval_accessor = c3_graph_node(process, spot, 55532, 0x17483c0);
    if (value<std::uintptr_t>(read<56>(process, c3_graph_node(process, spot, 103876, 0x1746848)).data(), 48) != pending ||
        value<std::uintptr_t>(read<32>(process, pending).data(), 16) != pending_accessor ||
        value<std::uintptr_t>(read<32>(process, pending).data(), 24) != approval ||
        value<std::uintptr_t>(read<32>(process, approval).data(), 16) != approval_accessor ||
        value<std::uintptr_t>(read<32>(process, approval).data(), 24) != result.shortcut_move)
        throw std::runtime_error("C1 shortcut approval/pending guard changed");
    c1_accessor_field(process, secrets.meta, pending_accessor, "Bool_id_tdgiayrv", 1, 0x17a1858, 0xc407ff);
    c1_accessor_field(process, secrets.meta, approval_accessor, "Bool_id_1x1elvoz", 1, 0x17a1858, 0xc407ff);
    const auto check_route = [&](std::uintptr_t address, std::uintptr_t original, std::uintptr_t override_route)
    {
        const auto current = value<std::uintptr_t>(read<8>(process, address).data(), 0);
        if (current != original && (!override_route || current != override_route))
            throw std::runtime_error("C1 map route has an unknown override. Stop other clients/tests first");
    };
    check_route(entry.entry_output, entry.original_output, entry_override);
    check_route(secrets.entry_output, secrets.original_output, secret_override);
    check_route(result.progression_route, result.original_progression, traversal ? result.bounds : 0);
    check_route(result.shortcut_route, result.shortcut_denied, traversal ? result.shortcut_move : 0);
    return result;
}

C1MapDiscovery find_c1_map(DWORD pid)
{
    const auto close = [](HANDLE h)
    { if (h) CloseHandle(h); };
    std::unique_ptr<void, decltype(close)> process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid), close);
    if (!process)
        throw std::runtime_error("Cannot open NST for C1 map discovery");
    const auto base = module_base(process.get());
    const auto &anchor = c1_anchor_spec();
    C1MapDiscovery result;
    const auto context = value<std::uintptr_t>(read<8>(process.get(), base + 0x1ac1dd8).data(), 0);
    const auto events_meta = value<std::uintptr_t>(read<8>(process.get(), base + 0x1a744a0).data(), 0);
    std::vector<std::uintptr_t> owners;
    const bool owners_available = hub_entity_owners(process.get(), base, context, events_meta, owners);
    const auto meta_context = value<std::uintptr_t>(read<8>(process.get(), base + 0x1acbe40).data(), 0);
    if (owners_available && metadata_registry(process.get(), meta_context).list)
    {
        for (const auto owner : owners)
        {
            const auto raw = read<144>(process.get(), owner);
            if (value<std::uintptr_t>(raw.data(), 0) != base + portal_vtable ||
                !std::equal(anchor.position.begin(), anchor.position.end(), raw.begin() + 32))
                continue;
            if (!c3_identity_matches(anchor, raw.data(),
                                     metadata_name(process.get(), value<std::uintptr_t>(raw.data(), 128)), base))
                continue;
            if (result.anchor && result.anchor != owner)
                throw std::runtime_error("Multiple C1 map owners found. Attachment refused");
            result.anchor = owner;
        }
        const auto scripts = script_registry(process.get(), base, meta_context);
        const auto manager = scripts.find("Scripts.Graph.common_Crash1_Hub_Manager");
        if (manager != scripts.end())
            result.manager_meta = manager->second;
        if (value<std::uintptr_t>(read<8>(process.get(), base + 0x1ac1dd8).data(), 0) != context ||
            value<std::uintptr_t>(read<8>(process.get(), base + 0x1acbe40).data(), 0) != meta_context)
            throw std::runtime_error("C1 registry context changed during discovery");
        if (result.anchor && result.manager_meta)
            probe_c1_map_at(process.get(), result);
        return result;
    }
    std::vector<unsigned char> buffer(1 << 20);
    for (std::uintptr_t address = 0; address < 0x7fff00000000ULL;)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process.get(), reinterpret_cast<void *>(address), &region, sizeof(region)))
            break;
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress), end = start + region.RegionSize;
        if (end <= address)
            break;
        if (readable(region))
            for (auto pos = start; pos < end; pos += std::min<std::size_t>(buffer.size(), end - pos))
            {
                SIZE_T count = 0;
                const auto length = std::min<std::size_t>(buffer.size(), end - pos);
                if (!ReadProcessMemory(process.get(), reinterpret_cast<void *>(pos), buffer.data(), length, &count))
                    continue;
                for (std::size_t offset = (8 - (pos & 7)) & 7; offset + 144 <= count; offset += 8)
                {
                    const auto vt = value<std::uintptr_t>(buffer.data() + offset, 0);
                    if (vt != base + portal_vtable && vt != base + 0x1745d78)
                        continue;
                    std::string name;
                    try
                    {
                        name = metadata_name(process.get(), value<std::uintptr_t>(buffer.data() + offset, vt == base + portal_vtable ? 128 : 16));
                    }
                    catch (const std::exception &)
                    {
                        continue;
                    }
                    auto *slot = vt == base + portal_vtable ? &result.anchor : &result.manager_meta;
                    if (vt == base + portal_vtable)
                    {
                        if (name != anchor.name || !std::equal(anchor.position.begin(), anchor.position.end(), buffer.begin() + offset + 32))
                            continue;
                    }
                    else if (name != "Scripts.Graph.common_Crash1_Hub_Manager")
                        continue;
                    if (*slot && *slot != pos + offset)
                        throw std::runtime_error("Multiple C1 map owners/manager graphs found. Attachment refused");
                    *slot = pos + offset;
                }
            }
        address = end;
    }
    if (result.anchor && result.manager_meta)
        probe_c1_map_at(process.get(), result);
    return result;
}

void verify_c1_map_restore(HANDLE process, const C1MapProbe &before)
{
    // graph roots can outlive map owners. validate when leaving the hub.
    for (const auto &[meta, name, fields] : {
             std::tuple{before.entry.meta, "Scripts.Graph.common_Crash1_Hub_Manager", 136u},
             std::tuple{before.secrets.meta, "Scripts.Graph.common_Crash1_Hub_MapSpot", 53u}})
    {
        const auto raw = read<104>(process, meta);
        if (value<std::uintptr_t>(raw.data(), 0) != module_base(process) + 0x1745d78 ||
            value<unsigned>(raw.data(), 24) != fields ||
            metadata_name(process, value<std::uintptr_t>(raw.data(), 16)) != name)
            throw std::runtime_error("C1 script allocation changed. Restoration refused");
    }
    const auto manager = before.entry.meta - 142276, spot = before.secrets.meta - 62812;
    if (c3_graph_node(process, manager, 174644, 0x1746848) + 48 != before.entry.entry_output ||
        c3_graph_node(process, manager, 174724, 0x13bc390) != before.entry.original_output ||
        c3_graph_node(process, spot, 71676, 0x1746848) + 48 != before.secrets.entry_output ||
        c3_graph_node(process, spot, 71868, 0x1745c08) != before.secrets.original_output ||
        c3_graph_node(process, spot, 104076, 0x1759168) + 32 != before.shortcut_route ||
        c3_graph_node(process, spot, 102260, 0x175dbd8) != before.shortcut_denied ||
        c3_graph_node(process, spot, 104116, 0x13bc390) != before.shortcut_move ||
        before.progression_route != manager + 159036 + 40 || before.original_progression != manager + 159092 ||
        before.bounds != manager + 159292 ||
        value<std::uintptr_t>(read<56>(process, c3_graph_node(process, manager, 158972, 0x1745c08)).data(), 48) != manager + 159036)
        throw std::runtime_error("C1 restore graph identities changed");
}

std::vector<unsigned char> c1_map_entry_chain(const C1MapProbe &probe, std::uintptr_t remote)
{
    if (probe.entry.destinations.size() != 34 || !probe.false_value || !probe.resume_input ||
        remote > std::numeric_limits<std::uintptr_t>::max() - (34 * 256 + 40 + 512))
        throw std::invalid_argument("Incomplete C1 map entry/recovery chain");
    std::set<std::uintptr_t> owners;
    for (unsigned i = 0; i < 34; ++i)
        if (probe.entry.destinations[i].key != c1_spots[i].key || !owners.insert(probe.entry.destinations[i].zone).second)
            throw std::invalid_argument("C1 map chain has missing/duplicate owners or keys");
    auto copy = c3_portal_entry_chain(probe.entry, remote);
    const auto recovery = remote + copy.size();
    // denied/unknown C1 selections clear loading and resume input instead of leaving the map frozen
    copy.insert(copy.end(), probe.loading_assignment.begin(), probe.loading_assignment.end());
    const auto pointer = [&](std::size_t offset, std::uintptr_t value)
    { std::memcpy(copy.data() + offset, &value, 8); };
    pointer(34 * 256 + 16, probe.false_value);
    pointer(34 * 256 + 32, probe.resume_input);
    for (unsigned i = 0; i < 34; ++i)
        pointer(i * 256 + 32, recovery);
    pointer(33 * 256 + 40, recovery); // unknown selection also resumes engine input
    return copy;
}

std::vector<unsigned char> c1_map_secret_chain(const C1MapProbe &probe, std::uintptr_t remote)
{
    if (probe.secrets.destinations.size() != 2 || !probe.secrets.owners[0])
        throw std::invalid_argument("Incomplete C1 secret-path chain");
    auto copy = c3_portal_entry_chain(probe.secrets, remote);
    for (unsigned i = 0; i < 2; ++i)
        std::memcpy(copy.data() + i * 256 + 32, &probe.secrets.owners[0], 8);
    std::memcpy(copy.data() + 256 + 40, &probe.secrets.original_output, 8);
    return copy;
}

namespace
{
    void gem_metadata(HANDLE process, std::uintptr_t address, const std::string &name,
                      unsigned count, bool data = false)
    {
        const auto raw = read<184>(process, address);
        const auto size = value<std::uint16_t>(raw.data(), 90);
        const auto base = module_base(process);
        if (value<std::uintptr_t>(raw.data(), 0) != base + (data ? 0x1745cc0 : 0x1745d78) ||
            metadata_name(process, value<std::uintptr_t>(raw.data(), 16)) != "Scripts.Graph." + name ||
            value<unsigned>(raw.data(), 24) != count || value<unsigned>(raw.data(), 32) < count * 8 ||
            value<std::uintptr_t>(raw.data(), 56) != base + (data ? 0x13bbb10 : 0x13bbbd0) ||
            size < (data ? 40 : 64) || size > 4096 || value<std::uint16_t>(raw.data(), 96))
            throw std::runtime_error("Gem graph metadata differs from the verified engine layout");
    }

    void gem_handle(HANDLE process, const std::array<unsigned char, 48> &raw, unsigned color)
    {
        const auto flags = value<unsigned>(raw.data(), 0);
        if ((flags & 0xf0000000) != 0x40000000 || (flags & 0x30000000) ||
            value<unsigned>(raw.data(), 16) != gem_hashes.at(color) ||
            value<unsigned>(raw.data(), 32) != 0x4a98ee98 ||
            metadata_name(process, value<std::uintptr_t>(raw.data(), 8)) != std::string("Gem_") + gem_colors.at(color) ||
            metadata_name(process, value<std::uintptr_t>(raw.data(), 24)) != "gamevariables")
            throw std::runtime_error("Gem variable handle is unresolved or belongs to another namespace");
        const auto variable = read<48>(process, value<std::uintptr_t>(raw.data(), 40));
        if (value<std::uintptr_t>(variable.data(), 0) != module_base(process) + 0x155ebf0 ||
            value<unsigned>(variable.data(), 16) != 0 || variable[24] != 0)
            throw std::runtime_error("Gem handle no longer resolves to its saved CGameBoolVariable");
    }

    void upgrade_handle(HANDLE process, std::uintptr_t address, unsigned item, std::uintptr_t base)
    {
        const auto raw = read<48>(process, address);
        if ((value<unsigned>(raw.data(), 0) & 0xf0000000) != 0x40000000 ||
            value<unsigned>(raw.data(), 16) != upgrade_hashes.at(item) ||
            value<unsigned>(raw.data(), 32) != 0x4a98ee98 ||
            metadata_name(process, value<std::uintptr_t>(raw.data(), 8)) != upgrade_names.at(item) ||
            metadata_name(process, value<std::uintptr_t>(raw.data(), 24)) != "gamevariables")
            throw std::runtime_error("Ability handle is unresolved or belongs to another variable");
        const auto variable = read<48>(process, value<std::uintptr_t>(raw.data(), 40));
        if (value<std::uintptr_t>(variable.data(), 0) != base + 0x155ebf0 ||
            value<unsigned>(variable.data(), 16) || variable[24])
            throw std::runtime_error("Ability handle no longer resolves to its saved engine variable");
    }

    std::array<std::uintptr_t, 6> ability_handles_at(HANDLE process, std::uintptr_t slab, std::uintptr_t base)
    {
        constexpr std::array<unsigned, 6> fields{{59404, 60428, 88596, 59916, 59660, 60172}};
        std::array<std::uintptr_t, 6> handles{};
        for (unsigned i = 0; i < fields.size(); ++i)
        {
            const auto field = slab + fields[i];
            if (value<std::uintptr_t>(read<8>(process, field).data(), 0) != base + 0x17a46b8)
                throw std::runtime_error("Ability default field differs from its verified runtime type");
            handles[i] = value<std::uintptr_t>(read<96>(process, field).data(), 88);
            upgrade_handle(process, handles[i], i, base);
        }
        for (unsigned i = 0; i < fields.size(); ++i)
            if (value<std::uintptr_t>(read<96>(process, slab + fields[i]).data(), 88) != handles[i])
                throw std::runtime_error("Ability default handles changed during discovery");
        return handles;
    }

}

GemTemplates probe_campaign_templates_at(HANDLE process, const GemDiscovery &found)
{
    if (!found.power_meta || found.power_meta < 67452)
        throw std::runtime_error("Campaign-check script graph is not loaded");
    gem_metadata(process, found.power_meta, "Crash_PowerUpgradeManager", 70);
    const auto slab = found.power_meta - 67452;
    const auto base = module_base(process);
    GemTemplates result;
    result.comparison_vtable = base + 0x1759220;
    result.campaign = read<64>(process, c3_graph_node(process, slab, 62716, 0x1745c08));
    result.enumeration = read<32>(process, c3_graph_node(process, slab, 62788, 0x1749820));
    result.outputs = read<56>(process, c3_graph_node(process, slab, 62820, 0x17452b0));
    result.constant = read<32>(process, c3_graph_node(process, slab, 49636, 0x1747690));
    result.data_accessor_vtable = value<std::uintptr_t>(read<24>(process, slab + 53020).data(), 0);
    if (result.data_accessor_vtable < base || result.data_accessor_vtable >= base + 0x1d74000)
        throw std::runtime_error("Canonical component-data accessor has no engine vtable");
    const auto first = value<std::uintptr_t>(result.campaign.data(), 32);
    const auto function = read<80>(process, value<std::uintptr_t>(result.campaign.data(), 56));
    if (value<unsigned>(result.campaign.data(), 16) != 1 || value<unsigned>(result.campaign.data(), 24) < 8 ||
        value<std::uintptr_t>(read<8>(process, first).data(), 0) != slab + 62788 ||
        value<std::uintptr_t>(result.campaign.data(), 40) != 0 ||
        value<std::uintptr_t>(result.campaign.data(), 48) != slab + 62820 ||
        value<unsigned>(result.enumeration.data(), 24) != 3 ||
        value<unsigned>(result.outputs.data(), 16) != 2 || value<unsigned>(result.outputs.data(), 24) < 16 ||
        value<std::uintptr_t>(result.outputs.data(), 32) != slab + 62820 + 40 ||
        value<std::uintptr_t>(result.outputs.data(), 40) != slab + 62876 ||
        value<std::uintptr_t>(result.outputs.data(), 48) != 0 ||
        value<std::uintptr_t>(function.data(), 0) != base + 0x17b50a8 ||
        metadata_name(process, value<std::uintptr_t>(function.data(), 16)) != "checkCrashGame_VSC")
        throw std::runtime_error("Engine Crash_Game dispatch templates changed");
    return result;
}

GemTemplates probe_gem_templates_at(HANDLE process, const GemDiscovery &found)
{
    auto result = probe_campaign_templates_at(process, found);
    for (unsigned i = 0; i < 6; ++i)
    {
        if (!found.handles[i])
            throw std::runtime_error("A resolved local gem variable is missing");
        result.handles[i] = *found.handles[i];
        gem_handle(process, result.handles[i], i);
    }
    return result;
}

GemDiscovery find_ability_metadata(DWORD pid)
{
    const auto close = [](HANDLE h)
    {if(h)CloseHandle(h); };
    std::unique_ptr<void, decltype(close)> process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid), close);
    if (!process)
        throw std::runtime_error("Cannot open NST for read-only ability discovery");
    const auto base = module_base(process.get());
    const auto context = value<std::uintptr_t>(read<8>(process.get(), base + 0x1acbe40).data(), 0);
    const auto scripts = script_registry(process.get(), base, context);
    GemDiscovery found;
    const auto lookup = [&](const char *name)
    {const auto it=scripts.find(name);return it==scripts.end()?0:it->second; };
    found.power_meta = lookup("Scripts.Graph.Crash_PowerUpgradeManager");
    if (!found.power_meta)
        return found;
    probe_campaign_templates_at(process.get(), found);
    const auto slab = found.power_meta - 67452;
    gem_metadata(process.get(), slab + 87964, "Crash_PowerUpgradeManagerData", 31, true);
    if (value<std::uintptr_t>(read<184>(process.get(), found.power_meta).data(), 176) != slab + 87964)
        throw std::runtime_error("Ability graph data binding changed during discovery");
    found.upgrades = ability_handles_at(process.get(), slab, base);
    found.ability_menu_meta = lookup("Scripts.Graph.common_Boolean_Game_Variable_Element");
    found.hud_meta = lookup("Scripts.Graph.CrashBandicoot_HUD");
    if (value<std::uintptr_t>(read<8>(process.get(), base + 0x1acbe40).data(), 0) != context ||
        script_registry(process.get(), base, context) != scripts)
        throw std::runtime_error("Ability metadata registry changed during discovery");
    found.registry_metadata = true;
    return found;
}

GemRouteProbe probe_gem_route_at(HANDLE process, const std::string &name,
                                 std::uintptr_t meta, std::uintptr_t override_route,
                                 std::uintptr_t data_accessor_vtable, std::uintptr_t effect_override)
{
    const auto it = std::find_if(gem_routes.begin(), gem_routes.end(),
                                 [&](const auto &s)
                                 { return name == s.name; });
    if (it == gem_routes.end() || meta < it->meta)
        throw std::invalid_argument("Unknown local gem-query graph");
    const auto &spec = *it;
    gem_metadata(process, meta, name, spec.fields);
    const auto slab = meta - spec.meta;
    gem_metadata(process, slab + spec.data_meta, name + "Data", spec.data_fields, true);
    if (value<std::uintptr_t>(read<184>(process, meta).data(), 176) != slab + spec.data_meta)
        throw std::runtime_error("Gem graph has another data metadata binding");
    GemRouteProbe result;
    result.name = name;
    result.meta = meta;
    const auto null = c3_graph_node(process, slab, spec.null_test, 0x17592d8);
    result.route = null + 24;
    result.original = c3_graph_node(process, slab, spec.test, 0x1759168);
    result.accessor = c3_graph_node(process, slab, spec.accessor, 0x1747910);
    const auto property = c3_graph_node(process, slab, spec.property, 0x1748780);
    result.yes = c3_graph_node(process, slab, spec.yes, spec.yes_type);
    result.no = c3_graph_node(process, slab, spec.no, spec.no_type);
    result.test = read<40>(process, result.original);
    if (value<std::uintptr_t>(read<24>(process, null).data(), 16) != result.accessor ||
        value<std::uintptr_t>(result.test.data(), 16) != property ||
        value<std::uintptr_t>(result.test.data(), 24) != result.yes ||
        value<std::uintptr_t>(result.test.data(), 32) != result.no ||
        value<std::uintptr_t>(read<32>(process, property).data(), 24) != result.accessor)
        throw std::runtime_error("Gem ownership query no longer matches the engine graph");
    const auto accessor = read<40>(process, result.accessor);
    if (value<std::uintptr_t>(accessor.data(), 24) != slab + spec.field ||
        value<std::uintptr_t>(accessor.data(), 32) != slab + spec.data_accessor ||
        !data_accessor_vtable || value<std::uintptr_t>(read<8>(process, slab + spec.data_accessor).data(), 0) != data_accessor_vtable)
        throw std::runtime_error("Gem query no longer reads component data");
    const auto field = read<72>(process, slab + spec.field);
    const auto offset = value<std::uint16_t>(field.data(), 24);
    const auto size = value<std::uint16_t>(read<104>(process, slab + spec.data_meta).data(), 90);
    if (offset < 40 || offset > size || size - offset < 8)
        throw std::runtime_error("Gem variable field is outside compiled component data");
    result.variable_offset = offset;
    result.data_meta = slab + spec.data_meta;
    result.variable_field = slab + spec.field;
    result.data_size = size;
    result.component_size = value<std::uint16_t>(read<104>(process, meta).data(), 90);
    result.meta_name = value<std::uintptr_t>(read<24>(process, meta).data(), 16);
    result.data_name = value<std::uintptr_t>(read<24>(process, result.data_meta).data(), 16);
    boss_field(process, slab + spec.data_meta, 0, "GemGameVariable", offset, 8,
               module_base(process) + 0x17a46b8, 0xc423ff);
    const auto current = value<std::uintptr_t>(read<8>(process, result.route).data(), 0);
    if (current != result.original && (!override_route || current != override_route))
        throw std::runtime_error("Gem ownership route has an unknown override");
    if (name == "Crash2_YellowGemTeleporter_Cover")
    {
        result.effect_route = c3_graph_node(process, slab, 11456, 0x1759168) + 24;
        result.effect_original = c3_graph_node(process, slab, 11496, 0x1745c08);
        result.effect_accessor = c3_graph_node(process, slab, 11920, 0x17477d0);
        const auto effect_field = c3_graph_node(process, slab, 10040, 0x17a46b8);
        const auto effect_offset = value<std::uint16_t>(read<32>(process, effect_field).data(), 24);
        if (value<std::uintptr_t>(read<32>(process, result.effect_accessor).data(), 24) != effect_field ||
            value<std::uintptr_t>(read<104>(process, result.effect_original).data(), 96) != result.effect_accessor)
            throw std::runtime_error("Yellow teleporter effect binding changed");
        boss_field(process, meta, 0, "SpawnedVFX_metaField_id_siwkjz5j", effect_offset, 8,
                   module_base(process) + 0x17a46b8, 0xc423ff);
        const auto effect = value<std::uintptr_t>(read<8>(process, result.effect_route).data(), 0);
        if (effect != result.effect_original && (!effect_override || effect != effect_override))
            throw std::runtime_error("Yellow teleporter effect has another override");
    }
    return result;
}

GemDiscovery find_gem_routes(DWORD pid, const GemDiscovery &cached)
{
    const auto close = [](HANDLE h)
    { if (h) CloseHandle(h); };
    std::unique_ptr<void, decltype(close)> process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid), close);
    if (!process)
        throw std::runtime_error("Cannot open NST for read-only gem discovery");
    const auto base = module_base(process.get());
    GemDiscovery found;
    const auto remember = [&](const std::string &name, std::uintptr_t meta)
    {
        std::uintptr_t *slot = nullptr;
        if (name == "Scripts.Graph.Crash_PowerUpgradeManager")
            slot = &found.power_meta;
        else if (name == "Scripts.Graph.common_Boolean_Game_Variable_Element")
            slot = &found.ability_menu_meta;
        else if (name == "Scripts.Graph.common_Crate_LevelCount")
            slot = &found.crate_meta;
        else if (name == "Scripts.Graph.CrashBandicoot_HUD")
            slot = &found.hud_meta;
        else
            for (const auto &spec : gem_routes)
                if (name == std::string("Scripts.Graph.") + spec.name)
                    slot = &found.graphs[spec.name];
        if (slot)
        {
            if (*slot && *slot != meta)
                throw std::runtime_error("Multiple engine graphs found: " + name);
            *slot = meta;
        }
    };
    try
    {
        const auto registry = script_registry(process.get(), base, value<std::uintptr_t>(read<8>(process.get(), base + 0x1acbe40).data(), 0));
        if (!registry.empty())
        {
            for (const auto &[name, meta] : registry)
            {
                remember(name, meta);
            }
            found.registry_metadata = true;
        }
    }
    catch (const std::exception &)
    {
        found = {};
    } // loading/reallocation falls back to the existing scan
    std::uintptr_t allocation = 0;
    if (cached.power_meta)
    {
        probe_gem_templates_at(process.get(), cached);
        if (!found.registry_metadata)
        {
            found.power_meta = cached.power_meta;
            found.ability_menu_meta = cached.ability_menu_meta;
        }
        found.handles = cached.handles;
        found.upgrades = cached.upgrades;
        found.secret_handle = cached.secret_handle;
        if (found.registry_metadata && found.power_meta && found.secret_handle &&
            std::all_of(found.upgrades.begin(), found.upgrades.end(), [](auto p)
                        { return p != 0; }))
        {
            probe_gem_templates_at(process.get(), found);
            probe_secret_variable(process.get(), *found.secret_handle);
            for (unsigned i = 0; i < 6; ++i)
                upgrade_handle(process.get(), found.upgrades[i], i, base);
            return found; // registry discovers new script allocations without scanning their bytes
        }
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process.get(), reinterpret_cast<void *>(cached.power_meta), &region, sizeof(region)))
            throw std::runtime_error("Engine gem metadata allocation disappeared");
        allocation = reinterpret_cast<std::uintptr_t>(region.AllocationBase);
    }
    // prescan the engine script allocation after bootstrap. another loader allocation needs a full rediscovery.
    std::vector<unsigned char> buffer(1024 * 1024 + 184);
    for (std::uintptr_t address = allocation; address < 0x7fff00000000ULL;)
    {
        MEMORY_BASIC_INFORMATION region{};
        if (!VirtualQueryEx(process.get(), reinterpret_cast<void *>(address), &region, sizeof(region)))
            break;
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress), end = start + region.RegionSize;
        if (end <= address || (allocation && reinterpret_cast<std::uintptr_t>(region.AllocationBase) != allocation))
            break;
        if (readable(region))
            for (auto pos = start; pos < end; pos += 1024 * 1024)
            {
                SIZE_T count = 0;
                const auto length = std::min<std::size_t>(buffer.size(), end - pos);
                if (!ReadProcessMemory(process.get(), reinterpret_cast<void *>(pos), buffer.data(), length, &count))
                    continue;
                for (std::size_t off = (8 - (pos & 7)) & 7; off + 184 <= count; off += 8)
                {
                    const auto raw = buffer.data() + off;
                    if (!found.registry_metadata && value<std::uintptr_t>(raw, 0) == base + 0x1745d78)
                    {
                        std::string name;
                        try
                        {
                            name = metadata_name(process.get(), value<std::uintptr_t>(raw, 16));
                        }
                        catch (const std::exception &)
                        {
                            continue;
                        }
                        remember(name, pos + off);
                    }
                    if (allocation || (value<unsigned>(raw, 0) & 0xf0000000) != 0x40000000 ||
                        value<unsigned>(raw, 32) != 0x4a98ee98)
                        continue;
                    if (value<unsigned>(raw, 16) == 0xeb8b471d)
                    {
                        const auto handle = read<48>(process.get(), pos + off);
                        try
                        {
                            probe_secret_variable(process.get(), handle);
                        }
                        catch (const std::exception &)
                        {
                            continue;
                        }
                        if (found.secret_handle && value<std::uintptr_t>(found.secret_handle->data(), 40) !=
                                                       value<std::uintptr_t>(handle.data(), 40))
                            throw std::runtime_error("Ambiguous engine secret-entry variable");
                        found.secret_handle = handle;
                    }
                    for (unsigned i = 0; i < 6; ++i)
                        if (value<unsigned>(raw, 16) == gem_hashes[i])
                        {
                            const auto handle = read<48>(process.get(), pos + off);
                            try
                            {
                                gem_handle(process.get(), handle, i);
                            }
                            catch (const std::exception &)
                            {
                                continue;
                            }
                            if (found.handles[i] && value<std::uintptr_t>(found.handles[i]->data(), 40) !=
                                                        value<std::uintptr_t>(handle.data(), 40))
                                throw std::runtime_error("Ambiguous local gem variable");
                            found.handles[i] = handle;
                        }
                }
            }
        address = end;
    }
    probe_gem_templates_at(process.get(), found);
    found.upgrades = ability_handles_at(process.get(), found.power_meta - 67452, base);
    return found;
}

std::int64_t gem_item_id(unsigned campaign, unsigned color)
{
    if (campaign < 1 || campaign > 3 || color >= 6 || (campaign != 1 && color == 2))
        return 0;
    return 0xCB0000 + 2000 + (campaign == 1 ? color : 6 + (campaign - 2) * 5 + color - (color > 2));
}

AbilityProbe probe_abilities_at(HANDLE process, const GemDiscovery &found, bool installed,
                                const std::array<unsigned char, 5> &jump_override,
                                const std::array<unsigned char, 5> &refresh_override)
{
    gem_metadata(process, found.power_meta, "Crash_PowerUpgradeManager", 70);
    if (found.power_meta < 67452)
        throw std::runtime_error("Invalid engine power metadata");
    const auto slab = found.power_meta - 67452, base = module_base(process);
    gem_metadata(process, slab + 87964, "Crash_PowerUpgradeManagerData", 31, true);
    if (value<std::uintptr_t>(read<184>(process, found.power_meta).data(), 176) != slab + 87964)
        throw std::runtime_error("Ability graph data binding changed");
    for (unsigned i = 0; i < 6; ++i)
        upgrade_handle(process, found.upgrades[i], i, base);
    AbilityProbe result;
    result.meta = found.power_meta;
    result.yes = c3_graph_node(process, slab, 70660, 0x1748280);
    result.no = c3_graph_node(process, slab, 70948, 0x1748280);
    if (read<32>(process, result.yes)[24] != 1 || read<32>(process, result.no)[24] != 0)
        throw std::runtime_error("Engine constant ability values changed");
    for (unsigned i = 0; i < ability_queries.size(); ++i)
    {
        const auto &spec = ability_queries[i];
        const auto test = read<40>(process, c3_graph_node(process, slab, spec.test, 0x1759168));
        result.inputs[i] = slab + spec.test + 16;
        result.original[i] = c3_graph_node(process, slab, spec.property, 0x1748780);
        const auto current = value<std::uintptr_t>(test.data(), 16);
        if ((current != result.original[i] && (!installed || (current != result.yes && current != result.no))) ||
            value<std::uintptr_t>(test.data(), 24) != c3_graph_node(process, slab, spec.yes, 0x1759168) ||
            value<std::uintptr_t>(test.data(), 32) != c3_graph_node(process, slab, spec.no, 0x1759168) ||
            value<std::uintptr_t>(read<32>(process, result.original[i]).data(), 24) != slab + spec.accessor)
            throw std::runtime_error("Ability query has changed or has another override");
        const auto accessor = read<40>(process, c3_graph_node(process, slab, spec.accessor, 0x1747910));
        if (value<std::uintptr_t>(accessor.data(), 24) != slab + spec.field ||
            value<std::uintptr_t>(accessor.data(), 32) != slab + 53020)
            throw std::runtime_error("Ability query no longer reads engine component data");
        const auto field = read<96>(process, slab + spec.field);
        const auto offset = value<std::uint16_t>(field.data(), 24);
        boss_field(process, slab + 87964, 0, spec.field_name, offset, 8, base + 0x17a46b8, 0xc423ff);
        // runtime field order differs from serialized character data. bind through its engine default handle.
        const auto handle = value<std::uintptr_t>(field.data(), 88);
        upgrade_handle(process, handle, spec.item, base);
        if (value<std::uintptr_t>(read<48>(process, handle).data(), 40) !=
            value<std::uintptr_t>(read<48>(process, found.upgrades[spec.item]).data(), 40))
            throw std::runtime_error("Ability query resolves another upgrade variable");
    }
    result.jump_variable = value<std::uintptr_t>(read<48>(process, found.upgrades[2]).data(), 40);
    result.jump_call = base + 0x4afb6a;
    result.getter = base + 0x4e5c60;
    result.refresh_call = base + 0x4af6b2;
    result.execute = base + 0xc988c0;
    result.actor_vtable = base + 0x1592220;
    result.component_vtable = base + 0x13bbbd0;
    constexpr std::array<unsigned char, 5> original_refresh_call{{0xe8, 0x59, 0x02, 0x32, 0}};
    constexpr std::array<unsigned char, 16> refresh_guard{{0x48, 0x8b, 0xf1, 0x48, 0x8b, 0x49, 0x18, 0x4c, 0x8b, 0xea, 0x0f, 0x29, 0x7c, 0x24, 0x50, 0x44}};
    constexpr std::array<unsigned char, 16> execute_guard{{0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x6b, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x70}};
    if (read<16>(process, base + 0x4af69d) != refresh_guard ||
        read<16>(process, result.execute) != execute_guard ||
        read<4>(process, base + 0x7cf910) != std::array<unsigned char, 4>{{0x48, 0x8b, 0xc1, 0xc3}} ||
        read<5>(process, result.refresh_call) != (refresh_override[0] ? refresh_override : original_refresh_call))
        throw std::runtime_error("Ability-cache refresh call differs from the pinned build or has another override");
    result.method = read<24>(process, c3_graph_node(process, slab, 69828, 0x17473c8));
    result.nodes = read<56>(process, c3_graph_node(process, slab, 69852, 0x1746090));
    result.entry = read<56>(process, c3_graph_node(process, slab, 69908, 0x1745fd0));
    if (value<std::uintptr_t>(result.method.data(), 16) != slab + 69852 ||
        value<unsigned>(result.nodes.data(), 16) != 2 || value<unsigned>(result.nodes.data(), 20) != 2 ||
        value<std::uintptr_t>(result.nodes.data(), 32) != slab + 69852 + 40 ||
        value<std::uintptr_t>(result.nodes.data(), 40) != slab + 69908 ||
        value<std::uintptr_t>(result.entry.data(), 48) != c3_graph_node(process, slab, 67292, 0x1745c08))
        throw std::runtime_error("Ability-cache refresh method binding changed");
    constexpr std::array<unsigned char, 16> jump_guard{{0x48, 0x8b, 0x88, 0x28, 0x06, 0, 0, 0x84, 0xdb, 0x75, 0x7e, 0x48, 0x85, 0xc9, 0x74, 0x79}};
    constexpr std::array<unsigned char, 5> getter_guard{{0xe9, 0xab, 0xe9, 0xff, 0xff}};
    if (read<16>(process, base + 0x4afb5a) != jump_guard || read<5>(process, result.getter) != getter_guard ||
        read<5>(process, result.jump_call) != (installed ? jump_override : original_jump_call))
        throw std::runtime_error("Engine Double Jump call differs from the pinned build or has another override");
    return result;
}

std::vector<unsigned char> movement_input_stub(std::uintptr_t blocked_controller)
{
    if (!blocked_controller)
        throw std::invalid_argument("Missing movement input state");
    // this getter is a leaf. keep direction registers untouched instead of calling C++ from it
    std::vector<unsigned char> code{
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, // address of the controller restricted to CB1 movement
        0x48, 0x3b, 0x08, 0x75, 0x1a,       // other controllers use the original getter
        0x83, 0xfa, 0x3e, 0x77, 0x15,       // keep bit indices outside the reflected XBUTTON range unchanged
        0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,
        0x48, 0x0f, 0xa3, 0xd0, 0x73, 0x05, // test the crouch/slide input mask
        0x8b, 0xca, 0x31, 0xc0, 0xc3,
        0x48, 0x8b, 0x41, 0x10, 0x8b, 0xca, 0x48, 0x8b, 0x40, 0x10,
        0x48, 0x0f, 0xa3, 0xc8, 0x0f, 0x92, 0xc0, 0xc3};
    // XBUTTON_B, R1, ATTACK_TERTIARY, B_LSHIFT and B_RCLICK all reach crouch/slide paths
    const std::uint64_t buttons = (1ULL << 5) | (1ULL << 13) | (1ULL << 27) | (1ULL << 57) | (1ULL << 59);
    std::memcpy(code.data() + 2, &blocked_controller, 8);
    std::memcpy(code.data() + 22, &buttons, 8);
    return code;
}

AbilityOwnership ability_ownership(const std::set<std::int64_t> &items, const std::set<char> &campaigns)
{
    AbilityOwnership result{};
    for (unsigned i = 0; i < result.size(); ++i)
        result[i] = campaigns.count(i == 0 ? '2' : '3') && items.count(0xCB0000 + 2100 + i);
    return result;
}

std::array<unsigned char, 5> ability_jump_call(std::uintptr_t call, std::uintptr_t target)
{
    // E8 rel32 measures its signed displacement from the instruction after the five-byte call
    if (!call || !target || call >= 0x7fff00000000ULL - 5 || target >= 0x7fff00000000ULL)
        throw std::invalid_argument("Invalid ability call instruction address");
    const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(call + 5);
    if (delta < INT32_MIN || delta > INT32_MAX)
        throw std::invalid_argument("Ability stub is outside call range");
    std::array<unsigned char, 5> result{{0xe8}};
    const auto displacement = static_cast<std::int32_t>(delta);
    std::memcpy(result.data() + 1, &displacement, 4);
    return result;
}

std::vector<unsigned char> ability_jump_stub(const AbilityProbe &probe, std::uintptr_t remote, std::uintptr_t jump_filter)
{
    // windows can load the local DLL callback above 0x7fff00000000
    if (!probe.jump_variable || !probe.getter || !remote || (remote & 0xffff) || remote >= 0x7fff00000000ULL - 8192 || jump_filter >= 0x800000000000ULL)
        throw std::invalid_argument("Incomplete engine Double Jump adapter");
    // only this caller is redirected: serialization and engine upgrade setters keep their getter
    // filter only the AP Double Jump result through the applied movement style
    std::vector<unsigned char> code{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0x48, 0x39, 0xc1, 0x75, 0x08,
                                    0x0f, 0xb6, 0x05, 0, 0, 0, 0};
    std::memcpy(code.data() + 2, &probe.jump_variable, 8);
    // movzx eax, byte [rip+disp32] reads ownership at remote+4096. RIP is remote+22 here.
    const std::int32_t displacement = 4096 - 22;
    std::memcpy(code.data() + 18, &displacement, 4);
    if (jump_filter)
    {
        code.insert(code.end(), {0x8b, 0xc8, 0x48, 0xb8}); // tail call preserves the original getter's stack/shadow space
        const auto pointer = reinterpret_cast<const unsigned char *>(&jump_filter);
        code.insert(code.end(), pointer, pointer + 8);
        code.insert(code.end(), {0xff, 0xe0});
    }
    else
        code.push_back(0xc3);
    code[14] = static_cast<unsigned char>(code.size() - 15);
    code.insert(code.end(), {0x48, 0xb8});
    const auto pointer = reinterpret_cast<const unsigned char *>(&probe.getter);
    code.insert(code.end(), pointer, pointer + 8);
    code.insert(code.end(), {0xff, 0xe0});
    return code;
}

std::vector<unsigned char> ability_refresh_method(const AbilityProbe &probe, std::uintptr_t remote)
{
    // relocate a one-entry VSC method: method +0, node list +32, pointer storage +72, entry node +80.
    std::vector<unsigned char> result(136);
    std::copy(probe.method.begin(), probe.method.end(), result.begin());
    std::copy(probe.nodes.begin(), probe.nodes.end(), result.begin() + 32);
    std::copy(probe.entry.begin(), probe.entry.end(), result.begin() + 80);
    const auto pointer = [&](unsigned off, std::uintptr_t v)
    { std::memcpy(result.data() + off, &v, 8); };
    pointer(16, remote + 32);
    pointer(64, remote + 72);
    pointer(72, remote + 80);
    const unsigned one = 1;
    const std::uint64_t size = (value<std::uint64_t>(result.data(), 56) & 0xffff000000000000ULL) | 8;
    std::memcpy(result.data() + 48, &one, 4);
    std::memcpy(result.data() + 52, &one, 4);
    std::memcpy(result.data() + 56, &size, 8);
    return result;
}

std::vector<unsigned char> ability_refresh_stub(const AbilityProbe &probe, std::uintptr_t remote, std::uintptr_t filler_tick)
{
    if (!probe.meta || !probe.execute || !probe.actor_vtable || !probe.component_vtable || !remote)
        throw std::invalid_argument("Incomplete engine power refresh adapter");
    // replace one identity-helper call in CPlayerJump's update
    std::vector<unsigned char> code;
    const auto emit = [&](std::initializer_list<unsigned char> b)
    { code.insert(code.end(), b); };
    const auto imm64 = [&](std::uintptr_t v)
    { const auto p=reinterpret_cast<const unsigned char*>(&v); code.insert(code.end(),p,p+8); };
    const auto branch = [&](unsigned char condition)
    { emit({0x0f,condition,0,0,0,0}); return code.size()-4; };
    const auto bind = [&](std::size_t at, std::size_t destination)
    {
        const auto delta = static_cast<std::int32_t>(destination - at - 4);
        std::memcpy(code.data() + at, &delta, 4);
    };
    const auto state = [&]
    { emit({0x49,0xbb}); imm64(remote+4096); };
    // preserve flags, volatile integer registers and XMM0..5. stack +0..31 is Windows x64 call shadow space
    emit({0x9c, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x48, 0x81, 0xec, 0xa0, 0, 0, 0});
    for (unsigned i = 0; i < 6; ++i)
        emit({0xf3, 0x0f, 0x7f, static_cast<unsigned char>(0x44 + i * 8), 0x24, static_cast<unsigned char>(32 + i * 16)});
    state();
    emit({0x41, 0x80, 0x7b, 0x19, 1});
    const auto disabled = branch(0x85);
    emit({0xb0, 1, 0x41, 0x86, 0x43, 0x18, 0x84, 0xc0});
    const auto busy = branch(0x85);
    emit({0x48, 0xb8});
    imm64(probe.actor_vtable);
    emit({0x48, 0x39, 0x01});
    const auto wrong_actor = branch(0x85);
    // a retained actor can still exist while its level/graphs are being torn down.
    emit({0x80, 0xb9, 0x55, 1, 0, 0, 1});
    const auto inactive_actor = branch(0x85);
    emit({0x48, 0x8b, 0x51, 0x18, 0x48, 0x85, 0xd2});
    const auto no_list = branch(0x84);
    emit({0x8b, 0x42, 0x10, 0x83, 0xf8, 1});
    const auto empty = branch(0x8c);
    emit({0x3d, 0, 1, 0, 0});
    const auto too_many = branch(0x8f);
    emit({0x4c, 0x8b, 0x42, 0x20, 0x4d, 0x85, 0xc0});
    const auto no_data = branch(0x84);
    emit({0x49, 0xba});
    imm64(probe.component_vtable);
    emit({0x49, 0xb9});
    imm64(probe.meta);
    const auto loop = code.size();
    emit({0x49, 0x8b, 0x10, 0x48, 0x85, 0xd2});
    const auto null_component = branch(0x84);
    emit({0x4c, 0x39, 0x12});
    const auto other_type = branch(0x85);
    emit({0x48, 0x39, 0x4a, 0x18});
    const auto other_owner = branch(0x85);
    emit({0x4c, 0x39, 0x4a, 0x38});
    const auto found = branch(0x84);
    const auto next = code.size();
    emit({0x49, 0x83, 0xc0, 8, 0xff, 0xc8});
    const auto repeat = branch(0x85);
    emit({0xe9, 0, 0, 0, 0});
    const auto missing = code.size() - 4;
    const auto match = code.size();
    // refresh on a new component or generation, then acknowledge only the generation actually executed
    emit({0x49, 0x39, 0x53, 0x10});
    const auto new_owner = branch(0x85);
    emit({0x41, 0x8b, 0x43, 4, 0x41, 0x3b, 0x43, 8});
    const auto current = branch(0x84);
    const auto refresh = code.size();
    emit({0x41, 0x8b, 0x43, 4, 0x89, 0x84, 0x24, 0x80, 0, 0, 0, 0x48, 0x89, 0x94, 0x24, 0x88, 0, 0, 0});
    emit({0x48, 0xb9});
    imm64(remote + 4352);
    emit({0x45, 0x33, 0xc0, 0x45, 0x33, 0xc9, 0x48, 0xb8});
    imm64(probe.execute);
    emit({0xff, 0xd0});
    state();
    emit({0x48, 0x8b, 0x84, 0x24, 0x88, 0, 0, 0, 0x49, 0x89, 0x43, 0x10,
          0x8b, 0x84, 0x24, 0x80, 0, 0, 0, 0x41, 0x89, 0x43, 8});
    const auto release = code.size();
    if (filler_tick)
    {
        emit({0x48, 0x8b, 0x8c, 0x24, 0xc8, 0, 0, 0, 0x48, 0xb8});
        imm64(filler_tick);
        emit({0xff, 0xd0});
        state();
    }
    emit({0x41, 0xc6, 0x43, 0x18, 0});
    const auto finish = code.size();
    for (unsigned i = 0; i < 6; ++i)
        emit({0xf3, 0x0f, 0x6f, static_cast<unsigned char>(0x44 + i * 8), 0x24, static_cast<unsigned char>(32 + i * 16)});
    emit({0x48, 0x81, 0xc4, 0xa0, 0, 0, 0, 0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x9d, 0x48, 0x8b, 0xc1, 0xc3});
    for (auto at : {disabled, busy})
        bind(at, finish);
    for (auto at : {wrong_actor, inactive_actor, no_list, empty, too_many, no_data, missing, current})
        bind(at, release);
    for (auto at : {null_component, other_type, other_owner})
        bind(at, next);
    bind(repeat, loop);
    bind(found, match);
    bind(new_owner, refresh);
    return code;
}

FillerProbe probe_filler_at(HANDLE process, std::uintptr_t hud_meta)
{
    FillerProbe p;
    if (!hud_meta)
        return p;
    gem_metadata(process, hud_meta, "CrashBandicoot_HUD", 51);
    if (hud_meta < 61548)
        throw std::runtime_error("Invalid player HUD allocation");
    const auto slab = hud_meta - 61548, base = module_base(process);
    p.meta = hud_meta;
    p.data_meta = slab + 80700;
    p.world_slot = base + 0x1a64690;
    p.camera_slot = base + 0x1a890e0;
    const auto camera_meta = value<std::uintptr_t>(read<8>(process, base + 0x1a891d0).data(), 0);
    if (!camera_meta)
        throw std::runtime_error("Game reward camera metadata is not loaded");
    const auto camera = read<104>(process, camera_meta);
    p.camera_vtable = base + 0x151d390;
    if (metadata_name(process, value<std::uintptr_t>(camera.data(), 16)) != "CCameraSystemInstance" ||
        value<std::uint16_t>(camera.data(), 90) != 976 || value<std::uintptr_t>(camera.data(), 56) != p.camera_vtable)
        throw std::runtime_error("Game reward camera layout differs");
    gem_metadata(process, p.data_meta, "CrashBandicoot_HUDData", 19, true);
    if (value<std::uintptr_t>(read<184>(process, hud_meta).data(), 176) != p.data_meta)
        throw std::runtime_error("Player HUD data binding differs");
    p.data_size = value<std::uint16_t>(read<104>(process, p.data_meta).data(), 90);
    if (p.data_size < 48 || p.data_size > 4096)
        throw std::runtime_error("Player HUD data size differs");
    p.data_vtable = base + 0x13bbb10;
    if (value<std::uintptr_t>(read<8>(process, base + 0x175dd48 + 176).data(), 0) != base + 0xcab020 ||
        read<16>(process, base + 0xcab020) != std::array<unsigned char, 16>{{0x48, 0x89, 0x5c, 0x24, 8, 0x48, 0x89, 0x6c, 0x24, 16, 0x48, 0x89, 0x74, 0x24, 24, 0x57}} ||
        value<std::uintptr_t>(read<8>(process, base + 0x1754ea0 + 0x130).data(), 0) != base + 0x11b7b20 ||
        read<4>(process, base + 0x11b7b20) != std::array<unsigned char, 4>{{0x8b, 0x41, 0x18, 0xc3}})
        throw std::runtime_error("Engine HUD reward arithmetic differs");
    const unsigned nodes[]{52556, 50916}, constants[]{52604, 50964}, accessors[]{45452, 42596}, fields[]{45484, 42628}, outputs[]{52636, 50996}, senders[]{52396, 50740};
    const char *field_names[]{"addLives", "addWumpa"};
    for (unsigned i = 0; i < 2; ++i)
    {
        p.adds[i] = read<48>(process, c3_graph_node(process, slab, nodes[i], 0x175dd48));
        p.constants[i] = read<32>(process, c3_graph_node(process, slab, constants[i], 0x1754ea0));
        p.show[i] = read<88>(process, c3_graph_node(process, slab, senders[i], 0x1624f10));
        if (value<std::uintptr_t>(p.show[i].data(), 72) != slab + nodes[i])
            throw std::runtime_error("Game reward HUD event no longer leads to its accumulator");
        const auto property = c3_graph_node(process, slab, accessors[i], 0x1754fe0);
        if (value<std::uintptr_t>(read<32>(process, property).data(), 24) != slab + fields[i] ||
            value<std::uintptr_t>(p.adds[i].data(), 40) != slab + outputs[i])
            throw std::runtime_error("Engine HUD reward destination differs");
        c1_accessor_field(process, p.meta, property, field_names[i], 4, 0x17a0ec0, 0xc413ff);
        if (value<std::uintptr_t>(p.adds[i].data(), 16) != slab + constants[i] ||
            value<std::uintptr_t>(p.adds[i].data(), 24) != property || value<std::uintptr_t>(p.adds[i].data(), 32) != property ||
            value<int>(p.constants[i].data(), 24) != 1)
            throw std::runtime_error("Engine HUD reward accumulator differs");
    }
    return p;
}

unsigned wumpa_amount(unsigned receipt_index)
{
    // Half award one fruit, a quarter two, and so on, capped at ten.
    std::uint64_t bits = receipt_index + 0x9e3779b97f4a7c15ULL;
    bits = (bits ^ (bits >> 30)) * 0xbf58476d1ce4e5b9ULL;
    bits = (bits ^ (bits >> 27)) * 0x94d049bb133111ebULL;
    bits ^= bits >> 31;
    unsigned amount = 1;
    while (amount < 10 && (bits & 1))
    {
        ++amount;
        bits >>= 1;
    }
    return amount;
}
std::vector<unsigned char> filler_method(const AbilityProbe &method, const FillerProbe &filler, unsigned kind, std::uintptr_t remote, unsigned amount)
{
    if (kind > 1 || !filler.meta || amount < 1 || amount > 10)
        throw std::invalid_argument("Invalid filler method");
    auto result = ability_refresh_method(method, remote);
    result.resize(312);
    std::copy(filler.adds[kind].begin(), filler.adds[kind].end(), result.begin() + 144);
    std::copy(filler.constants[kind].begin(), filler.constants[kind].end(), result.begin() + 192);
    std::copy(filler.show[kind].begin(), filler.show[kind].end(), result.begin() + 224);
    const auto pointer = [&](unsigned off, std::uintptr_t v)
    { std::memcpy(result.data() + off, &v, 8); };
    // follow the engine pickup's ShowWumpa -> addWumpa path so the GUI counter updates
    pointer(128, remote + 224);
    pointer(296, remote + 144);
    pointer(160, remote + 192);
    const int added = kind ? amount : 1;
    std::memcpy(result.data() + 216, &added, 4);
    return result;
}

std::vector<unsigned char> filler_stub(const AbilityProbe &actor, const FillerProbe &p, std::uintptr_t remote)
{
    if (!actor.actor_vtable || !actor.component_vtable || !actor.execute || !p.meta || !p.data_meta || !p.data_vtable ||
        !p.world_slot || !p.camera_slot || !p.camera_vtable || !remote)
        throw std::invalid_argument("Incomplete filler dispatch profile");
    std::vector<unsigned char> code;
    const auto emit = [&](std::initializer_list<unsigned char> bytes)
    { code.insert(code.end(), bytes); };
    const auto imm = [&](std::uintptr_t v)
    {const auto b=reinterpret_cast<const unsigned char*>(&v);code.insert(code.end(),b,b+8); };
    const auto branch = [&](unsigned char op)
    {emit({0x0f,op,0,0,0,0});return code.size()-4; };
    const auto bind = [&](std::size_t at, std::size_t to)
    {const auto d=static_cast<std::int32_t>(to-at-4);std::memcpy(code.data()+at,&d,4); };
    // called inside the existing register-preserving ability hook. resolve HUD from its live player.
    // RW+64: request/completed uint64, kind uint32, ready byte, origin zone pointer. Methods at RW+1024/+1408.
    emit({0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 32, 0x48, 0xbb});
    imm(remote + 4160);
    emit({0xc6, 0x43, 20, 0, 0x48, 0x85, 0xc9});
    std::vector<std::size_t> done{branch(0x84)};
    emit({0x48, 0xb8});
    imm(actor.actor_vtable);
    emit({0x48, 0x39, 0x01});
    done.push_back(branch(0x85));
    emit({0x80, 0xb9, 0x55, 1, 0, 0, 1});
    done.push_back(branch(0x85));
    // recheck engine loading on the game thread
    emit({0x48, 0xb8});
    imm(p.camera_slot);
    emit({0x48, 0x8b, 0x00, 0x48, 0x85, 0xc0});
    done.push_back(branch(0x84));
    emit({0x48, 0x8b, 0x40, 32, 0x48, 0x85, 0xc0});
    done.push_back(branch(0x84));
    emit({0x8b, 0x50, 16, 0x83, 0xfa, 1});
    done.push_back(branch(0x82));
    emit({0x83, 0xfa, 4});
    done.push_back(branch(0x87));
    emit({0x3b, 0x50, 20});
    done.push_back(branch(0x87));
    emit({0x48, 0x8b, 0x40, 32, 0x48, 0x85, 0xc0});
    done.push_back(branch(0x84));
    emit({0x48, 0x8b, 0x00, 0x48, 0x85, 0xc0});
    done.push_back(branch(0x84));
    emit({0x48, 0xba});
    imm(p.camera_vtable);
    emit({0x48, 0x39, 0x10});
    done.push_back(branch(0x85));
    emit({0x66, 0x81, 0xb8, 0x78, 1, 0, 0, 1, 1});
    done.push_back(branch(0x85));
    emit({0x48, 0x8b, 0x51, 24, 0x48, 0x85, 0xd2});
    done.push_back(branch(0x84));
    emit({0x8b, 0x42, 16, 0x3d, 0, 1, 0, 0});
    done.push_back(branch(0x87));
    emit({0x4c, 0x8b, 0x42, 32, 0x4d, 0x85, 0xc0});
    done.push_back(branch(0x84));
    const auto loop = code.size();
    emit({0x85, 0xc0});
    done.push_back(branch(0x84));
    emit({0x49, 0x8b, 0x10, 0x48, 0x85, 0xd2});
    std::vector<std::size_t> next{branch(0x84)};
    const auto compare = [&](unsigned offset, std::uintptr_t pointer)
    {
        emit({0x49,0xb9});imm(pointer);emit({0x4c,0x39,0x4a,static_cast<unsigned char>(offset)});next.push_back(branch(0x85)); };
    compare(0, actor.component_vtable);
    compare(56, p.meta);
    emit({0x48, 0x39, 0x4a, 24});
    next.push_back(branch(0x85));
    emit({0xf6, 0x42, 32, 1});
    next.push_back(branch(0x84));
    emit({0xf6, 0x42, 32, 24});
    next.push_back(branch(0x85));
    emit({0xf6, 0x42, 40, 1});
    next.push_back(branch(0x84));
    emit({0x4c, 0x8b, 0x52, 16, 0x4d, 0x85, 0xd2});
    next.push_back(branch(0x84));
    emit({0x49, 0xb9});
    imm(p.data_vtable);
    emit({0x4d, 0x39, 0x0a});
    next.push_back(branch(0x85));
    emit({0x49, 0xb9});
    imm(p.data_meta);
    emit({0x4d, 0x39, 0x4a, 32});
    next.push_back(branch(0x85));
    emit({0xc6, 0x43, 20, 1, 0x48, 0x8b, 0x33, 0x48, 0x3b, 0x73, 8});
    done.push_back(branch(0x84));
    emit({0x48, 0xb8});
    imm(p.world_slot);
    emit({0x48, 0x8b, 0x00, 0x48, 0x85, 0xc0});
    done.push_back(branch(0x84));
    emit({0x4c, 0x8b, 0x4b, 24, 0x4d, 0x85, 0xc9});
    done.push_back(branch(0x84));
    emit({0x4c, 0x39, 0x88, 0xb8, 0, 0, 0});
    done.push_back(branch(0x85));
    emit({0x8b, 0x43, 16, 0x83, 0xf8, 1});
    done.push_back(branch(0x87));
    emit({0x48, 0x69, 0xc8, 0x80, 1, 0, 0, 0x48, 0xb8});
    imm(remote + 5120);
    emit({0x48, 0x03, 0xc8});
    emit({0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9, 0x48, 0xb8});
    imm(actor.execute);
    emit({0xff, 0xd0, 0x48, 0x89, 0x73, 8});
    emit({0xe9, 0, 0, 0, 0});
    done.push_back(code.size() - 4);
    const auto advance = code.size();
    for (auto at : next)
        bind(at, advance);
    emit({0x49, 0x83, 0xc0, 8, 0xff, 0xc8, 0xe9, 0, 0, 0, 0});
    bind(code.size() - 4, loop);
    const auto end = code.size();
    for (auto at : done)
        bind(at, end);
    emit({0x48, 0x83, 0xc4, 32, 0x5f, 0x5e, 0x5b, 0xc3});
    return code;
}

AbilityMenuProbe probe_ability_menu_at(HANDLE process, const GemDiscovery &found, std::uintptr_t override_route)
{
    AbilityMenuProbe result;
    if (!found.ability_menu_meta)
        return result;
    result.meta = found.ability_menu_meta;
    if (result.meta < 4924)
        throw std::runtime_error("Invalid ability-menu metadata");
    const auto slab = result.meta - 4924, base = module_base(process);
    const auto meta = read<104>(process, result.meta);
    if (value<std::uintptr_t>(meta.data(), 0) != base + 0x1745d78 ||
        metadata_name(process, value<std::uintptr_t>(meta.data(), 16)) != "Scripts.Graph.common_Boolean_Game_Variable_Element" ||
        value<unsigned>(meta.data(), 24) != 5 || value<std::uintptr_t>(meta.data(), 56) != base + 0x1623970 ||
        value<std::uint16_t>(meta.data(), 90) != 48 || value<std::uint16_t>(meta.data(), 96))
        throw std::runtime_error("Ability-menu layout differs from the verified engine GUI graph");
    result.route = c3_graph_node(process, slab, 5628, 0x1745fd0) + 48;
    result.original = c3_graph_node(process, slab, 5684, 0x1624f10);
    result.accessor = c3_graph_node(process, slab, 4708, 0x17477d0);
    const auto field = c3_graph_node(process, slab, 4740, 0x17a46b8);
    boss_field(process, result.meta, 0, "Game_Bool_Variable_id_ikhvgar0", 40, 8, base + 0x17a46b8, 0xc423ff);
    const auto current = value<std::uintptr_t>(read<8>(process, result.route).data(), 0);
    if (current != result.original && (!override_route || current != override_route))
        throw std::runtime_error("Ability-menu route has another override");
    if (value<std::uintptr_t>(read<32>(process, result.accessor).data(), 24) != field)
        throw std::runtime_error("Ability-menu variable accessor changed");
    result.sender = read<88>(process, result.original);
    result.arguments = read<56>(process, c3_graph_node(process, slab, 5772, 0x1745070));
    const auto owner = c3_graph_node(process, slab, 5564, 0x1624dd0);
    const auto property = c3_graph_node(process, slab, 4660, 0x1748780);
    const auto message = read<72>(process, value<std::uintptr_t>(result.sender.data(), 16));
    if (metadata_name(process, value<std::uintptr_t>(message.data(), 16)) != "common_Boolean_Placeable_Display.New_Graph_Event_1_id_u8v9igbe" ||
        value<std::uintptr_t>(result.sender.data(), 24) != owner || value<std::uintptr_t>(result.sender.data(), 32) != slab + 5772 ||
        value<unsigned>(result.sender.data(), 40) != 2 || value<unsigned>(result.sender.data(), 48) < 16 ||
        result.sender[64] || value<std::uintptr_t>(result.sender.data(), 72) || value<std::uintptr_t>(result.sender.data(), 80) ||
        value<unsigned>(result.arguments.data(), 16) != 2 || value<unsigned>(result.arguments.data(), 20) != 2 ||
        value<unsigned>(result.arguments.data(), 24) < 16 || value<std::uintptr_t>(result.arguments.data(), 32) != slab + 5772 + 40 ||
        value<std::uintptr_t>(result.arguments.data(), 40) != owner || value<std::uintptr_t>(result.arguments.data(), 48) != property ||
        value<std::uintptr_t>(read<32>(process, property).data(), 24) != result.accessor)
        throw std::runtime_error("Ability-menu display event binding changed");
    if (!override_route)
    {
        if (!found.power_meta)
            throw std::runtime_error("Ability-menu constants need the verified engine power graph");
        result.yes = read<32>(process, c3_graph_node(process, found.power_meta - 67452, 70660, 0x1748280));
        result.no = read<32>(process, c3_graph_node(process, found.power_meta - 67452, 70948, 0x1748280));
        if (result.yes[24] != 1 || result.no[24] != 0)
            throw std::runtime_error("Ability-menu constants changed");
    }
    return result;
}

std::vector<unsigned char> ability_menu_chain(const GemTemplates &templates, const AbilityMenuProbe &menu,
                                              std::uintptr_t remote, const AbilityOwnership &owned)
{
    GemRouteProbe route;
    route.accessor = menu.accessor;
    route.original = menu.original;
    route.yes = remote + 3072;
    route.no = remote + 3232;
    std::copy_n(menu.sender.begin(), 16, route.test.begin());
    GemOwnership masks{};
    masks[1][0] = owned[0];
    for (unsigned i = 1; i < 6; ++i)
        masks[2][i] = owned[i];
    auto result = gem_route_chain(templates, route, remote, masks);
    // after the campaign tests, append true/false sender nodes at +3072/+3232 and their values at +3392/+3424
    result.resize(3456);
    const auto pointer = [&](unsigned off, std::uintptr_t v)
    { std::memcpy(result.data() + off, &v, 8); };
    for (unsigned i = 0; i < 2; ++i)
    {
        const unsigned off = 3072 + i * 160;
        std::copy(menu.sender.begin(), menu.sender.end(), result.begin() + off);
        std::copy(menu.arguments.begin(), menu.arguments.end(), result.begin() + off + 96);
        pointer(off + 32, remote + off + 96);
        pointer(off + 128, remote + off + 136);
        pointer(off + 144, remote + 3392 + i * 32);
    }
    std::copy(menu.yes.begin(), menu.yes.end(), result.begin() + 3392);
    std::copy(menu.no.begin(), menu.no.end(), result.begin() + 3424);
    return result;
}

GemOwnership gem_ownership(const std::set<std::int64_t> &items, const std::set<char> &campaigns)
{
    GemOwnership result{};
    for (unsigned game = 1; game <= 3; ++game)
        for (unsigned color = 0; color < 6; ++color)
        {
            const auto id = gem_item_id(game, color);
            result[game - 1][color] = campaigns.count('0' + game) && id && items.count(id);
        }
    return result;
}

std::vector<unsigned char> gem_route_chain(const GemTemplates &templates, const GemRouteProbe &probe,
                                           std::uintptr_t remote, const GemOwnership &owned)
{
    constexpr unsigned stride = 1024, bytes = 3 * stride;
    if (!remote || (remote & 7) || remote >= 0x7fff00000000ULL - bytes ||
        !templates.comparison_vtable || !probe.accessor || !probe.original || !probe.yes || !probe.no)
        throw std::invalid_argument("Incomplete local gem dispatch chain");
    std::set<std::uintptr_t> variables;
    for (const auto &handle : templates.handles)
    {
        const auto variable = value<std::uintptr_t>(handle.data(), 40);
        if (!variable || (variable & 7) || variable >= 0x7fff00000000ULL ||
            (value<unsigned>(handle.data(), 0) & 0x30000000) || !variables.insert(variable).second)
            throw std::invalid_argument("Engine gem dispatch needs six distinct resolved variables");
    }
    std::vector<unsigned char> result(bytes);
    const auto pointer = [&](unsigned offset, std::uintptr_t v)
    { std::memcpy(result.data() + offset, &v, 8); };
    for (unsigned game = 0; game < 3; ++game)
    {
        const auto offset = game * stride;
        // campaign dispatch occupies the first 256 bytes. six 128-byte color comparisons fill the rest.
        const auto node = remote + offset;
        std::copy(templates.campaign.begin(), templates.campaign.end(), result.begin() + offset);
        std::copy(templates.enumeration.begin(), templates.enumeration.end(), result.begin() + offset + 64);
        std::copy(templates.outputs.begin(), templates.outputs.end(), result.begin() + offset + 96);
        pointer(offset + 32, node + 152);
        pointer(offset + 48, node + 96);
        pointer(offset + 152, node + 64);
        const unsigned campaign = game + 1;
        std::memcpy(result.data() + offset + 64 + 24, &campaign, 4);
        pointer(offset + 96 + 32, node + 136);
        pointer(offset + 136, node + 256);
        pointer(offset + 144, game < 2 ? node + stride : probe.no);
        for (unsigned color = 0; color < 6; ++color)
        {
            const auto start = offset + 256 + color * 128;
            const auto comparison = remote + start;
            std::copy(probe.test.begin(), probe.test.end(), result.begin() + start);
            pointer(start, templates.comparison_vtable);
            pointer(start + 16, probe.accessor);
            pointer(start + 24, comparison + 48);
            // +32 selects the owned/unowned result for a match. +40 tests the next color or falls back to the game.
            pointer(start + 32, owned[game][color] ? probe.yes : probe.no);
            pointer(start + 40, color < 5 ? comparison + 128 : probe.original);
            std::copy(templates.constant.begin(), templates.constant.end(), result.begin() + start + 48);
            pointer(start + 48 + 24, comparison + 80);
            std::copy(templates.handles[color].begin(), templates.handles[color].end(), result.begin() + start + 80);
        }
    }
    if (probe.effect_route)
    {
        // TestObject(existing SpawnedVFX): leave existing effect alone, spawn only when absent.
        result.resize(3112);
        std::copy(probe.test.begin(), probe.test.end(), result.begin() + 3072);
        pointer(3072, value<std::uintptr_t>(probe.test.data(), 0) + 0x170);
        pointer(3088, probe.effect_accessor);
        pointer(3096, 0);
        pointer(3104, probe.effect_original);
    }
    return result;
}

GemRefreshProbe probe_gem_refresh_at(HANDLE process, const GemDiscovery &found,
                                     const std::array<unsigned char, 5> &override_call)
{
    probe_gem_templates_at(process, found);
    const auto base = module_base(process), slab = found.power_meta - 67452;
    GemRefreshProbe p;
    p.call = base + 0x4acbf9;
    p.context = base + 0x1ac1dd8;
    p.manager = base + 0x1a64690;
    p.campaign = base + 0x131400;
    p.list = base + 0xbfd560;
    p.release = base + 0xdf0e10;
    p.execute = base + 0xc988c0;
    p.component_vtable = base + 0x13bbbd0;
    p.data_vtable = base + 0x13bbb10;
    p.meta_vtable = base + 0x1745d78;
    // Spawned VSC instances are absent from their direct type lists. CEntityEvents owners are tracked.
    p.events_vtable = base + 0x14cb600;
    p.events_meta = value<std::uintptr_t>(read<8>(process, base + 0x1a744a0).data(), 0);
    constexpr std::array<unsigned char, 5> original_call{{0xe8, 0x12, 0x2d, 0x32, 0}};
    constexpr std::array<unsigned char, 16> caller{{0x48, 0x8b, 0x4f, 0x10, 0x48, 0x8b, 0xf0, 0xe8, 0x12, 0x2d, 0x32, 0, 0x48, 0x8b, 0xce, 0x48}};
    auto guard = read<16>(process, base + 0x4acbf2);
    // Only bytes +7..11 are replaceable here. the surrounding instructions still identify the expected caller.
    const auto expected = override_call[0] ? override_call : original_call;
    auto pinned = caller;
    std::copy(expected.begin(), expected.end(), pinned.begin() + 7);
    // compare the call independently. gotta make sure to not mask a foreign instruction replacement
    if (guard != pinned || read<5>(process, p.call) != expected ||
        read<8>(process, base + 0x44a9b0) != std::array<unsigned char, 8>{{0x48, 0x8b, 0x05, 0xe9, 0x9a, 0x62, 0x01, 0xc3}} ||
        value<std::uintptr_t>(read<8>(process, p.events_vtable).data(), 0) != base + 0x44a9b0 ||
        read<5>(process, base + 0x4512e0) != std::array<unsigned char, 5>{{0x48, 0x8b, 0x41, 0x38, 0xc3}} ||
        value<std::uintptr_t>(read<8>(process, p.component_vtable).data(), 0) != base + 0x4512e0 ||
        read<4>(process, base + 0x7cf910) != std::array<unsigned char, 4>{{0x48, 0x8b, 0xc1, 0xc3}} ||
        read<16>(process, p.list) != std::array<unsigned char, 16>{{0x48, 0x89, 0x5c, 0x24, 0x20, 0x4c, 0x89, 0x44, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48}} ||
        read<16>(process, p.release) != std::array<unsigned char, 16>{{0x48, 0x85, 0xc9, 0x0f, 0x85, 0x87, 0xcf, 0xfe, 0xff, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc}} ||
        read<16>(process, p.campaign) != std::array<unsigned char, 16>{{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0x0d, 0x80, 0xa2, 0x92, 0x01}} ||
        read<16>(process, p.execute) != std::array<unsigned char, 16>{{0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x6b, 0x18, 0x56, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x70}})
        throw std::runtime_error("Engine gem refresh functions differ from the pinned build or have another override");
    if (!p.events_meta || metadata_name(process, value<std::uintptr_t>(read<24>(process, p.events_meta).data(), 16)) != "CEntityEvents" ||
        value<std::uintptr_t>(read<64>(process, p.events_meta).data(), 56) != p.events_vtable)
        throw std::runtime_error("Engine entity-event registry metadata changed");
    p.method.method = read<24>(process, c3_graph_node(process, slab, 69828, 0x17473c8));
    p.method.nodes = read<56>(process, c3_graph_node(process, slab, 69852, 0x1746090));
    p.method.entry = read<56>(process, c3_graph_node(process, slab, 69908, 0x1745fd0));
    if (value<std::uintptr_t>(p.method.method.data(), 16) != slab + 69852 ||
        value<unsigned>(p.method.nodes.data(), 16) != 2 || value<unsigned>(p.method.nodes.data(), 20) != 2 ||
        value<std::uintptr_t>(p.method.nodes.data(), 32) != slab + 69892 ||
        value<std::uintptr_t>(p.method.nodes.data(), 40) != slab + 69908 ||
        value<std::uintptr_t>(p.method.entry.data(), 48) != slab + 67292)
        throw std::runtime_error("Engine gem refresh method templates changed");
    for (unsigned i = 0; i < 6; ++i)
        p.variables[i] = value<std::uintptr_t>(found.handles[i]->data(), 40);
    return p;
}

std::uintptr_t find_crate_metadata(DWORD pid, CrateMetadataPointer &cached)
{
    if (cached.pid != pid)
        cached = {};
    const auto close = [](HANDLE h)
    {if(h)CloseHandle(h); };
    std::unique_ptr<void, decltype(close)> process(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid), close);
    if (!process)
        throw std::runtime_error("Cannot open NST for crate metadata");
    const auto base = module_base(process.get());
    const auto meta = crate_metadata_pointer(process.get(), base, value<std::uintptr_t>(read<8>(process.get(), base + 0x1acbe40).data(), 0), cached);
    cached.pid = pid;
    return meta;
}

CrateProbe probe_crate_at(HANDLE process, std::uintptr_t meta, std::uintptr_t override_route)
{
    gem_metadata(process, meta, "common_Crate_LevelCount", 27);
    if (meta < 16952)
        throw std::runtime_error("Invalid crate script allocation");
    const auto slab = meta - 16952, base = module_base(process), data_meta = slab + 24872;
    gem_metadata(process, data_meta, "common_Crate_LevelCountData", 14, true);
    if (value<std::uintptr_t>(read<184>(process, meta).data(), 176) != data_meta)
        throw std::runtime_error("Crate data metadata binding changed");
    const auto field = read<72>(process, slab + 16048);
    const auto offset = value<std::uint16_t>(field.data(), 24);
    const auto size = value<std::uint16_t>(read<104>(process, data_meta).data(), 90);
    if (offset < 40 || offset >= size)
        throw std::runtime_error("Crate counting flag outside engine data");
    boss_field(process, data_meta, 0, "addToCrateTotal", offset, 1, base + 0x17a1858, 0xc407ff);
    const auto entry = c3_graph_node(process, slab, 18760, 0x1746848);
    CrateProbe result;
    result.meta = meta;
    result.base = base;
    result.add_offset = offset;
    result.route = entry + 48;
    result.original = c3_graph_node(process, slab, 18912, 0x1759168);
    const auto current = value<std::uintptr_t>(read<8>(process, result.route).data(), 0);
    if (current != result.original && (!override_route || current != override_route))
        throw std::runtime_error("Crate break listener already has an override");
    result.node = read<40>(process, c3_graph_node(process, slab, 20016, 0x175dbd8));
    result.vtable = read<184>(process, base + 0x175dbd8);
    if (value<std::uintptr_t>(result.vtable.data(), 176) != base + 0xcab970)
        throw std::runtime_error("Crate node execute ABI changed");
    return result;
}

std::vector<unsigned char> crate_break_stub(const CrateProbe &probe, std::uintptr_t queue)
{
    if (!probe.meta || !probe.base || !queue || probe.add_offset < 40 || probe.add_offset > 4095)
        throw std::invalid_argument("Invalid crate event addresses");
    std::vector<unsigned char> code;
    const auto emit = [&](std::initializer_list<unsigned char> bytes)
    { code.insert(code.end(), bytes); };
    const auto imm64 = [&](std::uint64_t value)
    {for(unsigned i=0;i<8;++i) code.push_back(value>>(8*i)); };
    const auto imm32 = [&](std::uint32_t value)
    {for(unsigned i=0;i<4;++i) code.push_back(value>>(8*i)); };
    const auto jump = [&](unsigned condition)
    {emit({0x0f,static_cast<unsigned char>(condition)}); auto at=code.size();imm32(0);return at; };
    const auto fix = [&](std::size_t at, std::size_t target)
    {auto delta=static_cast<std::int32_t>(target-at-4);std::memcpy(code.data()+at,&delta,4); };
    std::vector<std::size_t> exits;
    // engine VSC execute(node RCX, context RDX) -> next node RAX. preserve W64 nonvolatile registers.
    emit({0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x48, 0x83, 0xec, 0x30});
    emit({0x48, 0x89, 0xcb, 0x48, 0x8b, 0x02, 0x48, 0x85, 0xc0});
    exits.push_back(jump(0x84));
    emit({0x49, 0xba});
    imm64(probe.base + 0x13bbbd0);
    emit({0x4c, 0x39, 0x10});
    exits.push_back(jump(0x85));
    emit({0x49, 0xba});
    imm64(probe.meta);
    emit({0x4c, 0x39, 0x50, 0x38});
    exits.push_back(jump(0x85));
    emit({0x48, 0x8b, 0x78, 0x18, 0x48, 0x85, 0xff});
    exits.push_back(jump(0x84));
    emit({0x48, 0x8b, 0x40, 0x10, 0x48, 0x85, 0xc0});
    exits.push_back(jump(0x84));
    emit({0x80, 0xb8});
    imm32(probe.add_offset);
    emit({0x01});
    exits.push_back(jump(0x85));
    emit({0x48, 0x8b, 0xb7, 0x80, 0, 0, 0, 0x48, 0x85, 0xf6});
    exits.push_back(jump(0x84));
    if (probe.identity)
    {
        // resolve prefab identity on the game thread before engine destruction/loading retires it
        emit({0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x8b, 0xcf, 0x48, 0x8d, 0x54, 0x24, 0x20, 0x48, 0xb8});
        imm64(probe.identity);
        emit({0xff, 0xd0, 0x48, 0x85, 0xc0});
        exits.push_back(jump(0x84));
        emit({0x48, 0x8b, 0xf8, 0x48, 0x8b, 0x74, 0x24, 0x20});
    }
    // obtain the current zone through the engine's checked cache
    emit({0x48, 0xa1});
    imm64(probe.base + 0x1a5b690);
    emit({0x48, 0x85, 0xc0});
    exits.push_back(jump(0x84));
    emit({0x48, 0x89, 0xc1, 0x48, 0xb8});
    imm64(probe.base + 0x753d80);
    emit({0xff, 0xd0, 0x48, 0x85, 0xc0});
    exits.push_back(jump(0x84));
    emit({0x48, 0x8b, 0x50, 0x10, 0x48, 0x85, 0xd2});
    exits.push_back(jump(0x84));
    emit({0x49, 0xba});
    imm64(queue);
    // lock xadd reserves a seq num. low ten bits select one of 1024 records, each 256 bytes after the header page.
    emit({0xb8, 1, 0, 0, 0, 0xf0, 0x49, 0x0f, 0xc1, 0x02, 0x49, 0x89, 0xc5});
    emit({0x49, 0x2b, 0x42, 0x08, 0x48, 0x3d, 0, 0x04, 0, 0});
    const auto overflow = jump(0x83);
    emit({0x4c, 0x89, 0xe8, 0x25, 0xff, 0x03, 0, 0, 0x48, 0xc1, 0xe0, 0x08, 0x4d, 0x8d, 0xa4, 0x02, 0, 0x10, 0, 0});
    // snapshot position and both names before the engine destroys or resets anything
    emit({0x48, 0x8b, 0x47, 0x20, 0x49, 0x89, 0x44, 0x24, 0x10, 0x8b, 0x47, 0x28, 0x41, 0x89, 0x44, 0x24, 0x18});
    const auto copy_string = [&](unsigned destination, bool zone)
    {
        emit(zone ? std::initializer_list<unsigned char>{0x49, 0x89, 0xd0} : std::initializer_list<unsigned char>{0x49, 0x89, 0xf0});
        emit({0x4d, 0x8d, 0x8c, 0x24});
        imm32(destination);
        emit({0xb9, 0x5f, 0, 0, 0});
        const auto loop = code.size();
        emit({0x41, 0x8a, 0x00, 0x41, 0x88, 0x01, 0x84, 0xc0});
        const auto done = jump(0x84);
        emit({0x49, 0xff, 0xc0, 0x49, 0xff, 0xc1, 0xff, 0xc9});
        auto again = jump(0x85);
        fix(again, loop);
        emit({0x41, 0xc6, 0x01, 0});
        fix(done, code.size());
    };
    copy_string(32, false);
    copy_string(128, true);
    emit({0x4c, 0x89, 0xe8, 0x48, 0xff, 0xc0, 0x49, 0x87, 0x04, 0x24}); // Publish ticket only after payload.
    emit({0xe9});
    auto completed = code.size();
    imm32(0);
    fix(overflow, code.size());
    emit({0x41, 0xc7, 0x42, 0x10, 1, 0, 0, 0});
    const auto end = code.size();
    fix(completed, end);
    for (auto at : exits)
        fix(at, end);
    emit({0x48, 0x8b, 0x43, 0x20, 0x48, 0x83, 0xc4, 0x30, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e, 0x5b, 0xc3});
    return code;
}

std::vector<unsigned char> gem_refresh_stub(const GemRefreshProbe &p,
                                            const std::vector<GemRouteProbe> &jobs, std::uintptr_t remote,
                                            std::uintptr_t addon_tick)
{
    if (!remote || !p.context || !p.manager || !p.campaign || !p.list || !p.release || !p.execute ||
        !p.component_vtable || !p.data_vtable || !p.meta_vtable || !p.events_meta || !p.events_vtable || jobs.size() > 3)
        throw std::invalid_argument("Incomplete local gem refresh profile");
    for (const auto &j : jobs)
        if (!j.meta || !j.data_meta || !j.route || !j.original || !j.accessor ||
            !j.meta_name || !j.data_name || !j.dispatcher || !j.variable_field ||
            j.component_size < 64 || j.component_size > 4096 || j.data_size < 48 || j.data_size > 4096 ||
            j.variable_offset < 40 || j.variable_offset + 8 > j.data_size)
            throw std::invalid_argument("Incomplete local gem component profile");
    std::vector<unsigned char> c;
    const auto emit = [&](std::initializer_list<unsigned char> b)
    { c.insert(c.end(), b); };
    const auto imm = [&](std::uint64_t v)
    { const auto b=reinterpret_cast<const unsigned char*>(&v); c.insert(c.end(),b,b+8); };
    const auto u32 = [&](unsigned v)
    { const auto b=reinterpret_cast<const unsigned char*>(&v); c.insert(c.end(),b,b+4); };
    const auto branch = [&](unsigned char op)
    { emit({0x0f,op,0,0,0,0}); return c.size()-4; };
    const auto jump = [&]
    { emit({0xe9,0,0,0,0}); return c.size()-4; };
    const auto bind = [&](std::size_t at, std::size_t target)
    {
        const auto d = static_cast<std::int32_t>(target - at - 4);
        std::memcpy(c.data() + at, &d, 4);
    };
    const auto call = [&](std::uintptr_t v)
    { emit({0x48,0xb8}); imm(v); emit({0xff,0xd0}); };
    // emitters use absolute calls via RAX. conditional/ordinary branches use rel32 fixups within this buffer.
    // the replaced identity helper preserves flags and every register except RAX=RCX
    // six nonvolatile registers hold state/list/component/index/mask/context across engine calls
    // RW page: request/completed uint64 at 0/8, count uint32 at16, busy/enabled bytes at20/21
    // copied game script methods start at RW+512, each occupying256 bytes. the code page is RX
    emit({0x9c, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
          0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xec, 0xd0, 0, 0, 0});
    for (unsigned i = 0; i < 6; ++i)
        emit({0xf3, 0x0f, 0x7f, static_cast<unsigned char>(0x44 + i * 8), 0x24, static_cast<unsigned char>(32 + i * 16)});
    emit({0x48, 0xbb});
    imm(remote + 4096);
    emit({0x80, 0x7b, 21, 1});
    const auto disabled = branch(0x85);
    emit({0xb0, 1, 0x86, 0x43, 20, 0x84, 0xc0});
    const auto busy = branch(0x85);
    if (addon_tick)
        call(addon_tick); // AP UI, DeathLink and movement work on the engine callback thread
    emit({0x48, 0x8b, 0x03, 0x48, 0x3b, 0x43, 8});
    const auto current = branch(0x84);
    emit({0x48, 0x89, 0x84, 0x24, 0x88, 0, 0, 0, 0xc7, 0x84, 0x24, 0x98, 0, 0, 0, 0, 0, 0, 0});
    // counts at RW+24..40: registered owners, matching scripts, ready, valid profiles, matching colors
    for (unsigned off = 24; off <= 44; off += 4)
        emit({0xc7, 0x43, static_cast<unsigned char>(off), 0, 0, 0, 0});
    emit({0x48, 0xb8});
    imm(p.context);
    emit({0x4c, 0x8b, 0x30, 0x4d, 0x85, 0xf6});
    const auto no_context = branch(0x84);
    emit({0x48, 0xb8});
    imm(p.manager);
    emit({0x48, 0x8b, 0x08, 0x48, 0x85, 0xc9});
    const auto no_manager = branch(0x84);
    call(p.campaign);
    emit({0x89, 0x43, 44, 0xff, 0xc8, 0x83, 0xf8, 2});
    const auto bad_campaign = branch(0x87);
    emit({0x6b, 0xc8, 6, 0x44, 0x8b, 0xac, 0x24, 0x88, 0, 0, 0, 0x41, 0xd3, 0xed, 0x41, 0x83, 0xe5, 63,
          0x45, 0x85, 0xed});
    const auto no_colors = branch(0x84);
    for (unsigned n = 0; n < jobs.size(); ++n)
    {
        const auto &j = jobs[n];
        emit({0x4c, 0x89, 0xf1, 0x48, 0x8d, 0x94, 0x24, 0x80, 0, 0, 0, 0x49, 0xb8});
        imm(p.events_meta);
        call(p.list);
        emit({0x48, 0x8b, 0xbc, 0x24, 0x80, 0, 0, 0, 0x48, 0x85, 0xff});
        const auto absent = branch(0x84);
        emit({0x31, 0xf6});
        const auto loop = c.size();
        emit({0x8b, 0x47, 16, 0x3d, 0, 0x10, 0, 0});
        const auto too_many = branch(0x87);
        emit({0x39, 0xc6});
        const auto end = branch(0x83);
        emit({0x48, 0x8b, 0x57, 32, 0x48, 0x85, 0xd2});
        const auto no_data = branch(0x84);
        // resolve current owners through registered CEntityEvents, then inspect their live component lists
        // no spawned script/component pointer is queued from the external client
        emit({0x48, 0x8b, 0x0c, 0xf2, 0x48, 0x85, 0xc9});
        std::vector<std::size_t> next_owner{branch(0x84)};
        emit({0x48, 0xb8});
        imm(p.events_vtable);
        emit({0x48, 0x39, 0x01});
        next_owner.push_back(branch(0x85));
        emit({0x48, 0x8b, 0x49, 24, 0x48, 0x85, 0xc9});
        next_owner.push_back(branch(0x84));
        emit({0xf6, 0x41, 56, 4});
        next_owner.push_back(branch(0x84));
        emit({0xff, 0x43, 24, 0x48, 0x89, 0x8c, 0x24, 0xb0, 0, 0, 0, 0x48, 0x8b, 0x49, 24, 0x48, 0x85, 0xc9});
        next_owner.push_back(branch(0x84));
        emit({0x48, 0x89, 0x8c, 0x24, 0xa0, 0, 0, 0, 0xc7, 0x84, 0x24, 0xa8, 0, 0, 0, 0, 0, 0, 0});
        const auto component_loop = c.size();
        emit({0x48, 0x8b, 0x8c, 0x24, 0xa0, 0, 0, 0, 0x8b, 0x41, 16, 0x3d, 0, 1, 0, 0});
        next_owner.push_back(branch(0x87));
        emit({0x8b, 0x94, 0x24, 0xa8, 0, 0, 0, 0x39, 0xc2});
        next_owner.push_back(branch(0x83));
        emit({0x48, 0x8b, 0x49, 32, 0x48, 0x85, 0xc9});
        next_owner.push_back(branch(0x84));
        emit({0x4c, 0x8b, 0x24, 0xd1, 0x4d, 0x85, 0xe4});
        std::vector<std::size_t> skip{branch(0x84)};
        const auto component_cmp = [&](unsigned off, std::uintptr_t v)
        {
            emit({0x48, 0xb8});
            imm(v);
            emit({0x49, 0x39, 0x44, 0x24, static_cast<unsigned char>(off)});
            skip.push_back(branch(0x85));
        };
        component_cmp(0, p.component_vtable);
        component_cmp(56, j.meta);
        emit({0xff, 0x43, 28, 0x48, 0x8b, 0x84, 0x24, 0xb0, 0, 0, 0, 0x49, 0x39, 0x44, 0x24, 24});
        skip.push_back(branch(0x85));
        emit({0x41, 0x8b, 0x44, 0x24, 32, 0xa8, 1});
        skip.push_back(branch(0x84)); // Started.
        emit({0xa8, 24});
        skip.push_back(branch(0x85)); // Crashed/pending removal.
        emit({0x41, 0xf6, 0x44, 0x24, 40, 1});
        skip.push_back(branch(0x84)); // Initialized.
        emit({0x49, 0x8b, 0x44, 0x24, 24, 0x48, 0x85, 0xc0});
        skip.push_back(branch(0x84));
        emit({0xf6, 0x40, 56, 4});
        skip.push_back(branch(0x84)); // Spawned owner. Hidden owners still qualify.
        emit({0xff, 0x43, 32});
        emit({0x49, 0x8b, 0x54, 0x24, 16, 0x48, 0x85, 0xd2});
        skip.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(p.data_vtable);
        emit({0x48, 0x39, 0x02});
        skip.push_back(branch(0x85));
        emit({0x48, 0xb8});
        imm(j.data_meta);
        emit({0x48, 0x39, 0x42, 32});
        skip.push_back(branch(0x85));
        // Metadata is dereferenced only after the engine returned a live component with this key.
        const auto meta_cmp = [&](std::uintptr_t address, std::uintptr_t expected)
        {
            emit({0x48, 0xb9});
            imm(address);
            emit({0x48, 0xb8});
            imm(expected);
            emit({0x48, 0x39, 0x01});
            skip.push_back(branch(0x85));
        };
        meta_cmp(j.meta, p.meta_vtable);
        meta_cmp(j.meta + 16, j.meta_name);
        meta_cmp(j.meta + 176, j.data_meta);
        meta_cmp(j.data_meta + 16, j.data_name);
        meta_cmp(j.meta + 56, p.component_vtable);
        meta_cmp(j.data_meta + 56, p.data_vtable);
        const auto word_cmp = [&](std::uintptr_t address, unsigned expected)
        {
            emit({0x48, 0xb9});
            imm(address);
            emit({0x66, 0x81, 0x39,
                  static_cast<unsigned char>(expected), static_cast<unsigned char>(expected >> 8)});
            skip.push_back(branch(0x85));
        };
        word_cmp(j.meta + 90, j.component_size);
        word_cmp(j.data_meta + 90, j.data_size);
        word_cmp(j.meta + 96, 0);
        word_cmp(j.data_meta + 96, 0);
        word_cmp(j.variable_field + 24, j.variable_offset);
        meta_cmp(j.route - 24, value<std::uintptr_t>(j.test.data(), 0) + 0x170); // TestObject/TestBool vtable delta
        meta_cmp(j.route - 8, j.accessor);
        emit({0x48, 0xb9});
        imm(j.route);
        emit({0x48, 0x8b, 0x01, 0x48, 0xb9});
        imm(j.dispatcher);
        emit({0x48, 0x39, 0xc8});
        const auto own_route = branch(0x84);
        emit({0x48, 0xb9});
        imm(j.original);
        emit({0x48, 0x39, 0xc8});
        skip.push_back(branch(0x85));
        bind(own_route, c.size());
        emit({0x48, 0x8b, 0x82});
        u32(j.variable_offset);
        emit({0x48, 0x85, 0xc0});
        skip.push_back(branch(0x84));
        emit({0x8b, 0x08, 0x81, 0xe1, 0, 0, 0, 0xf0, 0x81, 0xf9, 0, 0, 0, 0x40});
        skip.push_back(branch(0x85));
        emit({0x48, 0x8b, 0x50, 40});
        emit({0xff, 0x43, 36});
        std::vector<std::size_t> selected;
        for (unsigned color = 0; color < 6; ++color)
        {
            emit({0x41, 0xf7, 0xc5});
            u32(1u << color);
            const auto unchanged = branch(0x84);
            emit({0x48, 0xb8});
            imm(p.variables[color]);
            emit({0x48, 0x39, 0xc2});
            selected.push_back(branch(0x84));
            bind(unchanged, c.size());
        }
        skip.push_back(jump());
        const auto refresh = c.size();
        emit({0xff, 0x43, 40});
        emit({0x48, 0xb9});
        imm(remote + 4608 + n * 256);
        emit({0x4c, 0x89, 0xe2, 0x45, 0x31, 0xc0, 0x45, 0x31, 0xc9});
        call(p.execute);
        emit({0xff, 0x84, 0x24, 0x98, 0, 0, 0});
        const auto next = c.size();
        emit({0xff, 0x84, 0x24, 0xa8, 0, 0, 0});
        const auto another_component = jump();
        bind(another_component, component_loop);
        const auto another_owner = c.size();
        emit({0xff, 0xc6});
        const auto again = jump();
        bind(again, loop);
        const auto release = c.size();
        emit({0x48, 0x89, 0xf9});
        call(p.release);
        for (auto at : skip)
            bind(at, next);
        for (auto at : next_owner)
            bind(at, another_owner);
        for (auto at : selected)
            bind(at, refresh);
        for (auto at : {too_many, end, no_data})
            bind(at, release);
        bind(absent, c.size());
    }
    const auto ack = c.size();
    emit({0x8b, 0x84, 0x24, 0x98, 0, 0, 0, 0x89, 0x43, 16,
          0x48, 0x8b, 0x84, 0x24, 0x88, 0, 0, 0, 0x48, 0x89, 0x43, 8});
    const auto release = c.size();
    emit({0xc6, 0x43, 20, 0});
    const auto finish = c.size();
    for (unsigned i = 0; i < 6; ++i)
        emit({0xf3, 0x0f, 0x6f, static_cast<unsigned char>(0x44 + i * 8), 0x24, static_cast<unsigned char>(32 + i * 16)});
    emit({0x48, 0x81, 0xc4, 0xd0, 0, 0, 0, 0x41, 0x5e, 0x41, 0x5d, 0x41, 0x5c, 0x5f, 0x5e, 0x5b,
          0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5a, 0x59, 0x9d, 0x48, 0x8b, 0xc1, 0xc3});
    for (auto at : {disabled, busy})
        bind(at, finish);
    for (auto at : {current, no_context, no_manager, bad_campaign})
        bind(at, release);
    bind(no_colors, ack);
    if (c.size() > 4096)
        throw std::invalid_argument("Gem refresh code exceeds its RX page");
    return c;
}

std::vector<unsigned char> c3_button_event_chain(const std::vector<C3ButtonEventProbe> &probes, std::uintptr_t remote)
{
    // button slot: owner test +0, enable +160, activation message +240, contact test +320, cleanup +384
    if (probes.empty() || probes.size() > 36 || !remote || (remote & 7) ||
        remote > std::numeric_limits<std::uintptr_t>::max() - probes.size() * 512)
        throw std::invalid_argument("Invalid C3 event chain size/address");
    std::vector<unsigned char> result(probes.size() * 512);
    for (std::size_t i = 0; i < probes.size(); ++i)
    {
        const auto &probe = probes[i];
        if (!probe.entity || !probe.entry_output || !probe.original_output || !probe.owner_accessor || !probe.busy_check || !probe.contact_output || !probe.original_contact ||
            probe.entry_output != probes[0].entry_output || probe.original_output != probes[0].original_output ||
            probe.owner_accessor != probes[0].owner_accessor || probe.busy_check != probes[0].busy_check ||
            !probe.visibility_outputs[0] || !probe.visibility_outputs[1] || !probe.original_visibility[0] || !probe.original_visibility[1] ||
            probe.visibility_outputs != probes[0].visibility_outputs || probe.original_visibility != probes[0].original_visibility ||
            probe.contact_output != probes[0].contact_output || probe.original_contact != probes[0].original_contact)
            throw std::invalid_argument("C3 buttons do not share the verified activation graph");
        for (std::size_t j = 0; j < i; ++j)
            if (probe.entity == probes[j].entity)
                throw std::invalid_argument("Duplicate C3 activation entity");
        auto copy = probe.templates;
        const auto address = remote + i * 512;
        const auto pointer = [&](unsigned offset, std::uintptr_t value)
        { std::memcpy(copy.data() + offset, &value, 8); };
        pointer(16, probe.owner_accessor);
        pointer(24, address + 64);
        pointer(32, address + 384); // hide unowned/paired buttons through their engine cleanup event
        pointer(40, i + 1 < probes.size() ? address + 512 : probe.original_output);
        pointer(88, address + 96);
        pointer(136, probe.entity);
        pointer(312, probe.original_contact); // after the game activation message, retain original contact animation
        std::copy(copy.begin(), copy.begin() + 48, copy.begin() + 320);
        pointer(352, 0);
        pointer(360, i + 1 < probes.size() ? remote + (i + 1) * 512 + 320 : probe.original_contact);
        pointer(192, probe.busy_check); // preserve the busy/teleport state
        pointer(416, address + 424);
        std::copy(copy.begin(), copy.end(), result.begin() + i * 512);
    }
    return result;
}

C3PlatformProbe probe_c3_platform_at(HANDLE process, std::uintptr_t button, const std::string &key, bool placed)
{
    const auto &anchor = c3_spec(key);
    const auto raw = read<144>(process, button);
    const auto base = module_base(process);
    if (!c3_identity_matches(anchor, raw.data(), metadata_name(process, value<std::uintptr_t>(raw.data(), 128)), base) ||
        !(value<unsigned>(raw.data(), 56) & 0x100) || button < anchor.asset_offset)
        throw std::runtime_error("C3 secret platform anchor is inactive or changed");
    const auto hub = button - anchor.asset_offset;
    C3PlatformProbe result;
    for (unsigned i = 0; i < c3_platforms.size(); ++i)
    {
        const auto &spec = c3_platforms[i];
        result.entities[i] = hub + spec.asset_offset;
        const auto entity = read<144>(process, result.entities[i]);
        if (value<std::uintptr_t>(entity.data(), 0) != base + portal_vtable ||
            metadata_name(process, value<std::uintptr_t>(entity.data(), 128)) != spec.name ||
            ((placed || i == 2) && !std::equal(spec.position.begin(), spec.position.end(), entity.begin() + 32)) ||
            value<std::uintptr_t>(entity.data(), 64) != hub + spec.data_offset)
            throw std::runtime_error("C3 secret platform owner/data identity changed");
        // igEntity._bitfield._enabled is bit 8
        result.active[i] = (value<unsigned>(entity.data(), 56) & 0x100) != 0;
    }
    const auto &gate = c3_barriers[0];
    const auto owner = hub + gate.asset_offset;
    const auto entity = read<144>(process, owner);
    if (value<std::uintptr_t>(entity.data(), 0) != base + 0x141d490 ||
        metadata_name(process, value<std::uintptr_t>(entity.data(), 128)) != gate.name ||
        !std::equal(gate.position.begin(), gate.position.end(), entity.begin() + 32))
        throw std::runtime_error("C3 platform property owner changed");
    const auto behavior = c3_behavior(process, owner, "Scripts.Graph.L300_Hub_C3_Hub_ElectricGate", 176, 30, 152, 19);
    const auto slab = behavior[2] - 23764;
    result.false_value = c3_graph_node(process, slab, 20156, 0x1748280);
    // the button's IsEnabled property is read-only, reuse the gate's writable Enabled property
    result.enabled_property = read<48>(process, c3_graph_node(process, slab, 20188, 0x1748780));
    if (read<1>(process, result.false_value + 24)[0] ||
        !value<std::uintptr_t>(result.enabled_property.data(), 32) ||
        !value<std::uintptr_t>(result.enabled_property.data(), 40))
        throw std::runtime_error("C3 platform enabled property changed");
    return result;
}

std::vector<unsigned char> c3_barrier_event_chain(const C3ButtonEventProbe &button,
                                                 const std::array<C3BarrierEventProbe, 4> &barriers,
                                                 std::uintptr_t remote)
{
    if (!remote || (remote & 7) || remote > std::numeric_limits<std::uintptr_t>::max() - 4 * 256)
        throw std::invalid_argument("Invalid C3 gate dispatch address");
    std::vector<unsigned char> copied(4 * 256);
    // the four electric gates share a contact graph, dispatch by its physical owner
    for (unsigned i = 0; i < barriers.size(); ++i)
    {
        if (!barriers[i].entity || !barriers[i].owner_accessor || barriers[i].meta != barriers[0].meta)
            throw std::invalid_argument("Invalid C3 gate owner or shared graph");
        for (unsigned j = 0; j < i; ++j)
            if (barriers[i].entity == barriers[j].entity)
                throw std::invalid_argument("Duplicate C3 gate owner");
        auto *raw = copied.data() + i * 256;
        std::copy_n(button.templates.begin(), 144, raw);
        const auto address = remote + i * 256;
        const auto pointer = [&](unsigned offset, std::uintptr_t value)
        { std::memcpy(raw + offset, &value, 8); };
        pointer(16, barriers[i].owner_accessor);
        pointer(24, address + 64);
        pointer(32, 0);
        pointer(40, i + 1 < barriers.size() ? address + 256 : 0);
        pointer(88, address + 96);
        pointer(136, barriers[i].entity);
    }
    return copied;
}

std::vector<unsigned char> c3_platform_event_copy(const C3ButtonEventProbe &button,
                                                  const C3PlatformProbe &platform,
                                                  std::uintptr_t remote, std::uintptr_t continuation)
{
    if (!remote || (remote & 7) || !platform.false_value ||
        !value<std::uintptr_t>(button.templates.data(), 176) ||
        !value<std::uintptr_t>(platform.enabled_property.data(), 0) ||
        !value<std::uintptr_t>(platform.enabled_property.data(), 32) || !value<std::uintptr_t>(platform.enabled_property.data(), 40) ||
        remote > std::numeric_limits<std::uintptr_t>::max() - 3 * 256)
        throw std::invalid_argument("Invalid C3 secret platform action addresses");
    std::vector<unsigned char> copied(3 * 256);
    // per-platform slot: assignment +0, Enabled property +48, constant +96 and explicit entity handle +128
    for (unsigned i = 0; i < c3_platforms.size(); ++i)
    {
        if (!platform.entities[i] || (platform.entities[i] & 7))
            throw std::invalid_argument("Invalid C3 secret platform owner address");
        for (unsigned j = 0; j < i; ++j)
            if (platform.entities[i] == platform.entities[j])
                throw std::invalid_argument("Duplicate C3 platform owner");
        auto *raw = copied.data() + i * 256;
        const auto address = remote + i * 256;
        std::copy(button.templates.begin() + 160, button.templates.begin() + 200, raw); // Engine assignment
        std::copy(platform.enabled_property.begin(), platform.enabled_property.end(), raw + 48);
        std::copy(button.templates.begin() + 64, button.templates.begin() + 96, raw + 96);
        std::copy(button.templates.begin() + 96, button.templates.begin() + 144, raw + 128);
        const auto pointer = [&](unsigned offset, std::uintptr_t value)
        { std::memcpy(raw + offset, &value, 8); };
        pointer(16, c3_platforms[i].enabled ? value<std::uintptr_t>(button.templates.data(), 176) : platform.false_value);
        pointer(24, address + 48);
        pointer(32, i + 1 < c3_platforms.size() ? address + 256 : continuation);
        pointer(48 + 24, address + 96); // igEntity.Enabled recipient is an explicit constant handle, independent of script fields
        pointer(96 + 24, address + 128);
        pointer(128 + 40, platform.entities[i]);
    }
    return copied;
}

std::uintptr_t nst_module_base(HANDLE process) { return module_base(process); }

std::uintptr_t probe_secret_variable(HANDLE process, const std::array<unsigned char, 48> &handle)
{
    const auto base = module_base(process);
    if ((value<unsigned>(handle.data(), 0) & 0xf0000000) != 0x40000000 ||
        value<unsigned>(handle.data(), 16) != 0xeb8b471d || value<unsigned>(handle.data(), 32) != 0x4a98ee98 ||
        metadata_name(process, value<std::uintptr_t>(handle.data(), 8)) != "SecretPortal_ID" ||
        metadata_name(process, value<std::uintptr_t>(handle.data(), 24)) != "gamevariables")
        throw std::runtime_error("Secret-entry variable handle differs");
    const auto meta = value<std::uintptr_t>(read<8>(process, base + 0x1a91bf8).data(), 0);
    const auto metadata = read<104>(process, meta);
    const auto variable = value<std::uintptr_t>(handle.data(), 40);
    const auto object = read<48>(process, variable);
    if (metadata_name(process, value<std::uintptr_t>(metadata.data(), 16)) != "CGameIntVariable" ||
        value<std::uint16_t>(metadata.data(), 90) != 48 ||
        value<std::uintptr_t>(object.data(), 0) != value<std::uintptr_t>(metadata.data(), 56) ||
        read<5>(process, base + 0x68b830) != std::array<unsigned char, 5>{{0xe9, 0x9b, 0x76, 0xe7, 0xff}})
        throw std::runtime_error("Engine secret-entry setter or variable type differs");
    return variable;
}

bool c1_entrance_progression_valid(const std::string &level, int index)
{
    // stormy Ascent has no position in the regular map progression. the game uses -1 for it..
    return level == "L128" ? index == -1 : index >= 0 && index <= 34;
}

std::vector<ZoneRecord> read_zone_registry(HANDLE process)
{
    // zone manager global -> list at +40 , list count/capacity at +16/+20 , elements at +32
    const auto base = module_base(process);
    const auto manager = value<std::uintptr_t>(read<8>(process, base + 0x1a5b690).data(), 0);
    const auto list = read<40>(process, value<std::uintptr_t>(read<48>(process, manager).data(), 40));
    const auto count = value<unsigned>(list.data(), 16), capacity = value<unsigned>(list.data(), 20);
    if (!count)
        throw std::runtime_error("Zone registry is not loaded");
    if (count > 4096 || capacity < count)
        throw std::runtime_error("Invalid zone registry");
    const auto data = value<std::uintptr_t>(list.data(), 32);
    std::vector<ZoneRecord> result;
    for (unsigned i = 0; i < count; ++i)
    {
        const auto address = value<std::uintptr_t>(read<8>(process, data + 8 * i).data(), 0);
        const auto zone = read<128>(process, address);
        if (value<std::uintptr_t>(zone.data(), 0) != base + 0x13d0fb0)
            throw std::runtime_error("Zone registry contains another type");
        const auto path = metadata_name(process, value<std::uintptr_t>(zone.data(), 16));
        const auto debug = metadata_name(process, value<std::uintptr_t>(zone.data(), 32));
        if (path.rfind("crash", 0) != 0 || debug.rfind("crash", 0) != 0)
            continue;
        const auto offset = debug.find("/bosses/") == 6 ? 14 : 7;
        if (debug.size() < offset + 4 || debug[5] < '1' || debug[5] > '3' || debug[6] != '/' ||
            value<unsigned>(zone.data(), 120) != static_cast<unsigned>(debug[5] - '1' + 7))
            throw std::runtime_error("Engine zone campaign metadata differs");
        auto key = debug.substr(offset, 4);
        if ((key[0] != 'l' && key[0] != 'b') || key[1] != debug[5] ||
            key[2] < '0' || key[2] > '9' || key[3] < '0' || key[3] > '9')
            continue;
        key[0] = key[0] == 'l' ? 'L' : 'B';
        result.push_back({key, path, address});
    }
    return result;
}

std::vector<EntranceAnchor> entrance_anchors()
{
    std::vector<EntranceAnchor> result;
    auto add = [&](const char *hub, const char *key, const auto &bytes)
    {
        EntranceAnchor entrance;
        entrance.hub = hub;
        entrance.level = std::string(key).substr(0, 4);
        std::memcpy(entrance.position.data(), bytes.data(), 12);
        result.push_back(entrance);
    };
    for (const auto &spot : c1_spots)
        if (std::string(spot.key) != "L127")
            add("L100", spot.key, spot.position);
    for (const auto &portal : portals)
        add("L200", portal.level, portal.position);
    for (const auto &button : c3_buttons)
        add("L300", button.key, button.position);
    return result;
}

ActiveZone read_active_zone(HANDLE process, std::uintptr_t base)
{
    if (!base)
        base = module_base(process);
    ActiveZone result;
    result.world = value<std::uintptr_t>(read<8>(process, base + 0x1a64690).data(), 0);
    const auto manager = value<std::uintptr_t>(read<8>(process, base + 0x1a5b690).data(), 0);
    if (!result.world || !manager)
        return {};
    result.zone_name = value<std::uintptr_t>(read<8>(process, result.world + 184).data(), 0);
    const auto cache = read<16>(process, manager + 72);
    const auto handle = value<std::uintptr_t>(cache.data(), 0), cached_name = value<std::uintptr_t>(cache.data(), 8);
    if (!result.zone_name || !handle || !cached_name)
        return {};
    const auto weak = read<48>(process, handle);
    if (value<unsigned>(weak.data(), 0) & 0x30000000)
        return {};
    result.info = value<std::uintptr_t>(weak.data(), 40);
    if (!result.info)
        return {};
    const auto info = read<128>(process, result.info);
    if (value<std::uintptr_t>(info.data(), 0) != base + 0x13d0fb0)
        throw std::runtime_error("Active zone has an unexpected engine type");
    result.zone = metadata_name(process, result.zone_name);
    const auto path = value<std::uintptr_t>(info.data(), 16);
    if (!path || result.zone != metadata_name(process, cached_name) || result.zone != metadata_name(process, path))
        return {};
    auto normalized = result.zone;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c)
                   { return std::tolower(c); });
    if (normalized.rfind("maps/", 0) == 0)
        normalized.erase(0, 5);
    const auto kind = value<unsigned>(info.data(), 120);
    if (kind < 7 || kind > 9 || normalized.size() < 7 || normalized.rfind("crash", 0) != 0 || normalized[6] != '/' ||
        normalized[5] != '1' + kind - 7)
        return {};
    result.campaign = kind - 6;
    const auto offset = normalized.compare(7, 7, "bosses/") == 0 ? 14u : 7u;
    const auto folder = normalized.substr(offset, normalized.find('/', offset) - offset);
    if (folder.size() >= 4 && (folder[0] == 'l' || folder[0] == 'b') && folder[1] == normalized[5] &&
        std::isdigit(static_cast<unsigned char>(folder[2])) && std::isdigit(static_cast<unsigned char>(folder[3])) &&
        (folder.size() == 4 || folder[4] == '_'))
    {
        result.key = folder.substr(0, 4);
        result.key[0] = static_cast<char>(std::toupper(result.key[0]));
    }
    if (folder == "l" + std::to_string(result.campaign) + "00_hub")
        result.hub = result.campaign;
    result.entity_context = value<std::uintptr_t>(read<8>(process, base + 0x1ac1dd8).data(), 0);
    result.script_context = value<std::uintptr_t>(read<8>(process, base + 0x1acbe40).data(), 0);
    // the getter refreshes this cache when CWorld's active name changes, can be stale.
    if (read<16>(process, manager + 72) != cache || read<48>(process, handle) != weak ||
        value<std::uintptr_t>(read<8>(process, result.world + 184).data(), 0) != result.zone_name ||
        value<std::uintptr_t>(read<8>(process, base + 0x1a64690).data(), 0) != result.world ||
        value<std::uintptr_t>(read<8>(process, base + 0x1a5b690).data(), 0) != manager)
        return {};
    return result;
}

bool death_link_level(const ActiveZone &zone)
{
    return zone.campaign && zone.entity_context && zone.script_context && !zone.hub && !zone.key.empty();
}
SceneView read_scene(HANDLE process)
{
    const auto base = module_base(process);
    SceneView result;
    const auto world = value<std::uintptr_t>(read<8>(process, base + 0x1a64690).data(), 0);
    if (!world)
        return result;
    const auto zone = value<std::uintptr_t>(read<8>(process, world + 184).data(), 0);
    if (!zone)
        return result;
    result.zone_address = zone;
    result.zone = metadata_name(process, zone);
    const auto system = value<std::uintptr_t>(read<8>(process, base + 0x1a890e0).data(), 0);
    const auto meta = value<std::uintptr_t>(read<8>(process, base + 0x1a891d0).data(), 0);
    if (!system || !meta)
        return result;
    const auto metadata = read<104>(process, meta);
    if (metadata_name(process, value<std::uintptr_t>(metadata.data(), 16)) != "CCameraSystemInstance" ||
        value<std::uint16_t>(metadata.data(), 90) != 976)
        throw std::runtime_error("Camera instance layout differs");
    const auto fields = value<unsigned>(metadata.data(), 24);
    if (!fields || fields > 128)
        throw std::runtime_error("Invalid camera metadata fields");
    bool matrix_field = false;
    for (unsigned i = 0; i < fields; ++i)
    {
        const auto field = read<64>(process, value<std::uintptr_t>(read<8>(process,
                                                                           value<std::uintptr_t>(metadata.data(), 40) + i * 8)
                                                                       .data(),
                                                                   0));
        if (metadata_name(process, value<std::uintptr_t>(field.data(), 48)) != "_viewProjectionMatrix")
            continue;
        if (matrix_field || value<std::uint16_t>(field.data(), 22) != 64 || value<std::uint16_t>(field.data(), 24) != 512)
            throw std::runtime_error("Camera projection field differs");
        matrix_field = true;
    }
    if (!matrix_field)
        throw std::runtime_error("Camera projection field missing");
    const auto list = read<40>(process, value<std::uintptr_t>(read<40>(process, system).data(), 32));
    const auto count = value<unsigned>(list.data(), 16);
    if (!count || count > 4 || value<unsigned>(list.data(), 20) < count)
        return result;
    const auto instance = value<std::uintptr_t>(read<8>(process, value<std::uintptr_t>(list.data(), 32)).data(), 0);
    const auto raw = read<576>(process, instance);
    if (value<std::uintptr_t>(raw.data(), 0) != value<std::uintptr_t>(metadata.data(), 56) || !raw[376] || !raw[377])
        return result;
    std::memcpy(result.view_projection.data(), raw.data() + 512, 64);
    for (const auto entry : result.view_projection)
        if (!std::isfinite(entry))
            return result;
    result.camera_active = std::any_of(result.view_projection.begin(), result.view_projection.end(), [](float x)
                                       { return x != 0; });
    return result;
}
