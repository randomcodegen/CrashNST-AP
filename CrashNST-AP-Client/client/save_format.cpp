#include "save_format.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>

namespace save_probe
{
    namespace
    {
        constexpr std::size_t max_size = 8 * 1024 * 1024;

        std::uint32_t u32(const std::vector<std::uint8_t> &data, std::size_t offset)
        {
            if (offset > data.size() || data.size() - offset < 4)
                throw std::runtime_error("Save field outside file");
            return static_cast<std::uint32_t>(data[offset]) |
                   static_cast<std::uint32_t>(data[offset + 1]) << 8 |
                   static_cast<std::uint32_t>(data[offset + 2]) << 16 |
                   static_cast<std::uint32_t>(data[offset + 3]) << 24;
        }
    } // namespace

    std::string level_key(const std::string &zone)
    {
        if (zone.size() < 11 || zone.compare(0, 5, "crash") != 0 ||
            zone[5] < '1' || zone[5] > '3' || zone[6] != '/')
            return {};
        const auto offset = zone.compare(7, 7, "bosses/") == 0 ? 14 : 7;
        const auto end = zone.find('/', offset);
        const auto part = zone.substr(offset, end == std::string::npos ? end : end - offset);
        if (part.size() < 4 || (part[0] != 'l' && part[0] != 'b') ||
            part[1] != zone[5] || !std::all_of(part.begin() + 1, part.begin() + 4, [](char c)
                                               { return c >= '0' && c <= '9'; }) ||
            (part.size() > 4 && part[4] != '_'))
            return {};
        std::string key = part.substr(0, 4);
        key[0] = static_cast<char>(key[0] - 'a' + 'A');
        if (key[2] == '0' && key[3] == '0')
            return {}; // hub/intro records are not gameplay checks
        return key;
    }

    std::vector<std::uint8_t> read_file(const std::filesystem::path &path)
    {
        const auto size = std::filesystem::file_size(path);
        if (size < 512 || size > max_size)
            throw std::runtime_error("Unexpected save size");
        const auto modified = std::filesystem::last_write_time(path);
        std::ifstream file(path, std::ios::binary);
        if (!file)
            throw std::runtime_error("Cannot open save");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        file.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
        if (file.gcount() != static_cast<std::streamsize>(data.size()) ||
            std::filesystem::file_size(path) != size ||
            std::filesystem::last_write_time(path) != modified)
            throw std::runtime_error("Save changed while being read");
        return data;
    }

    std::map<char, std::string> recent_slots(const std::vector<std::uint8_t> &data)
    {
        // summary descriptors start at 0x38. each 12-byte record is type, name offset and value offset.
        if (data.size() < 512 || data.size() > max_size || u32(data, 0x30) != 3 ||
            u32(data, 0x34) < 5 * 12 || u32(data, 0x34) > 8 * 12 || u32(data, 0x34) % 12)
            throw std::runtime_error("Unknown SAVE_OPTIONS summary");
        const auto descriptors = u32(data, 0x34);
        const std::size_t names_at = 0x38 + descriptors + 4;
        const auto names_size = u32(data, names_at - 4);
        if (names_size > 4096 || names_at + names_size + 4 > data.size())
            throw std::runtime_error("Invalid options names");
        const auto values_at = names_at + names_size + 4;
        const auto values_size = u32(data, values_at - 4);
        if (values_size > 4096 || values_at + values_size > data.size())
            throw std::runtime_error("Invalid options values");
        const std::string fields[] = {"brightness_value", "subtitles_enabled", "autosave_enabled", "keyboard_mappings", "save_version",
                                      "most_recent_save_slot_crash1", "most_recent_save_slot_crash2", "most_recent_save_slot_crash3"};
        const unsigned kinds[] = {1, 0, 0, 3, 0, 0, 0, 0};
        std::map<char, std::string> result;
        unsigned seen = 0;
        // NST appends recent slots as campaigns are opened. descriptor order is not fixed.
        for (unsigned i = 0; i < descriptors / 12; ++i)
        {
            const auto descriptor = 0x38 + i * 12;
            const auto name_offset = u32(data, descriptor + 4), value_offset = u32(data, descriptor + 8);
            const auto end = i + 1 < descriptors / 12 ? u32(data, descriptor + 12 + 8) : values_size;
            if (name_offset >= names_size)
                throw std::runtime_error("Invalid options field name offset");
            const auto first = data.begin() + names_at + name_offset, last = data.begin() + names_at + names_size;
            const auto terminator = std::find(first, last, 0);
            if (terminator == last)
                throw std::runtime_error("Invalid options field name");
            const std::string name(first, terminator);
            const auto field = std::find(std::begin(fields), std::end(fields), name);
            if (field == std::end(fields))
                throw std::runtime_error("Unknown options field: " + name);
            const auto index = static_cast<unsigned>(field - std::begin(fields));
            if ((seen & (1u << index)) || u32(data, descriptor) != kinds[index] ||
                value_offset >= end || end > values_size || (index != 3 && end - value_offset != 4))
                throw std::runtime_error("Invalid options field: " + name);
            seen |= 1u << index;
            if (index >= 5)
            {
                const auto slot = u32(data, values_at + value_offset);
                if (slot == 0xffffffff)
                    continue;
                if (slot > 999)
                    throw std::runtime_error("Recent save index out of range");
                result.emplace(static_cast<char>('1' + index - 5), "SAVE_" + std::to_string(slot) + ".sav");
            }
        }
        if ((seen & 31) != 31)
            throw std::runtime_error("Missing options preference field");
        return result;
    }

    bool license_accepted(const std::vector<std::uint8_t> &data)
    {
        recent_slots(data);
        // pinned NST options schema. unknown layouts must keep the game license-agreement prompt
        if (u32(data, 12) != 28 || u32(data, 44) != 0xb000000c || u32(data, 4) > 128 || !u32(data, 4) ||
            u32(data, 0) > 4096 || u32(data, 0) < u32(data, 4) * 20)
            throw std::runtime_error("Unknown options state schema");
        const std::string first_type = "__internalObjectBase";
        const auto first = std::search(data.begin(), data.end(), first_type.begin(), first_type.end());
        if (first == data.end())
            throw std::runtime_error("Options type table missing");
        std::size_t names_end = 0;
        std::vector<std::pair<std::size_t, unsigned>> type_names;
        for (const char *name : {"__internalObjectBase", "igObject", "igNamedObject", "igDirEntry", "igObjectDirEntry",
                                 "igMemoryDirEntry", "igExternalDirEntry", "igExternalIndexedEntry", "igExternalInfoEntry", "igExternalObjectEntry",
                                 "igContainer", "igDataList", "igUnsignedIntList", "igReferenceResolver", "igInfo", "igObjectDirectoryIGBInfo",
                                 "igObjectList", "igNameList", "CAccoladeSystemSaveData", "CGameStateSaveData", "igBoolList", "igIntList",
                                 "igFloatList", "CCollectibleTrackerSystemSaveData", "igStringRefList", "CCollectibleTrackerSaveDataList",
                                 "igUnsignedIntListList", "igInfoList"})
        {
            const std::string marker = std::string(name) + '\0';
            const auto found = std::search(first, data.end(), marker.begin(), marker.end());
            if (found == data.end())
                throw std::runtime_error("Unknown options state types");
            type_names.emplace_back(found - data.begin(), type_names.size());
            names_end = std::max(names_end, found - data.begin() + marker.size());
        }
        std::sort(type_names.begin(), type_names.end());
        std::array<unsigned, 28> types{};
        for (unsigned i = 0; i < type_names.size(); ++i)
            types[type_names[i].second] = i;
        std::size_t accepted_at = 0;
        // directory entries refer to serialized type IDs, so resolve them from names instead of fixed numeric IDs.
        const auto directory_size = u32(data, 0), template_count = u32(data, 4);
        for (std::size_t start = names_end; start + directory_size + 12 < data.size(); ++start)
        {
            if (!((u32(data, start) == types[4] && u32(data, start + 4) == 20) ||
                  (u32(data, start) == types[5] && u32(data, start + 4) == 32)))
                continue;
            try
            {
                struct Entry
                {
                    unsigned type, count, width;
                    std::size_t at = 0;
                };
                std::vector<Entry> templates, entries;
                auto pos = start;
                for (unsigned i = 0; i < template_count; ++i)
                {
                    const auto kind = u32(data, pos), length = u32(data, pos + 4);
                    if (u32(data, pos + 8) != 0)
                        throw std::runtime_error("Invalid directory flags");
                    if (kind == types[4] && length == 20 && u32(data, pos + 12) > 0 && u32(data, pos + 12) < 28)
                        templates.push_back({u32(data, pos + 12), 0, 0});
                    else if (kind == types[5] && length == 32 && u32(data, pos + 12) <= 4096 &&
                             (u32(data, pos + 24) == 1 || u32(data, pos + 24) == 4 || u32(data, pos + 24) == 8))
                        templates.push_back({0, u32(data, pos + 12), u32(data, pos + 16) == 0 ? 4 : u32(data, pos + 24)});
                    else
                        throw std::runtime_error("Invalid directory entry");
                    pos += length;
                }
                if (pos != start + directory_size)
                    continue;
                const auto count = u32(data, pos + 4);
                if (!count || count > 4096 || u32(data, pos) != 8 + count * 2 || pos + 8 + count * 2 + 4 > data.size())
                    continue;
                for (unsigned i = 0; i < count; ++i)
                {
                    const auto at = pos + 8 + i * 2;
                    const unsigned index = data[at] | unsigned(data[at + 1]) << 8;
                    if (index >= templates.size())
                        throw std::runtime_error("Invalid directory reference");
                    entries.push_back(templates[index]);
                }
                pos += 8 + count * 2;
                if (u32(data, pos) >= count)
                    continue;
                pos += 4;
                std::size_t game = 0;
                for (auto &entry : entries)
                    if (entry.type)
                    {
                        const auto length = u32(data, pos + 4);
                        if (u32(data, pos) != entry.type || length < 8 || length > 4096 || length % 4 || length > data.size() - pos)
                            throw std::runtime_error("Invalid state object");
                        entry.at = pos;
                        if (entry.type == types[19])
                        {
                            if (game)
                                throw std::runtime_error("Duplicate game state");
                            game = pos;
                        }
                        pos += length;
                    }
                for (auto &entry : entries)
                    if (!entry.type)
                    {
                        const auto length = entry.count * entry.width;
                        if (pos > data.size() || length > data.size() - pos)
                            throw std::runtime_error("Truncated state buffer");
                        entry.at = pos;
                        pos += length;
                    }
                if (pos != data.size() || !game || u32(data, game + 4) != 24)
                    continue;
                const auto ref = u32(data, game + 8), ints = u32(data, game + 12);
                if (ref >= count || ints >= count || entries[ref].type != types[20] || entries[ints].type != types[21])
                    continue;
                const auto list = entries[ref].at, int_list = entries[ints].at;
                if (u32(data, list + 4) != 20 || u32(data, list + 8) != 8 || u32(data, list + 12) != 8 ||
                    u32(data, int_list + 8) != 435)
                    continue;
                const auto buffer = u32(data, list + 16);
                if (buffer >= count || entries[buffer].type || entries[buffer].count != 8 || entries[buffer].width != 1)
                    continue;
                const auto at = entries[buffer].at;
                if (!std::all_of(data.begin() + at, data.begin() + at + 8, [](auto value)
                                 { return value <= 1; }))
                    continue;
                if (accepted_at)
                    throw std::logic_error("Ambiguous options state");
                // game save/load visits keys in order. live keys are 379,380,381,443,444,445,460,463.
                accepted_at = at + 6;
            }
            catch (const std::runtime_error &)
            {
                continue;
            }
        }
        if (!accepted_at)
            throw std::runtime_error("Options acceptance state not found");
        return data[accepted_at] != 0;
    }

    Zones zones(const std::vector<std::uint8_t> &data)
    {
        // the summary's descriptor/name/value sections precede a length-prefixed table of zone path strings.
        if (data.size() < 512 || data.size() > max_size || u32(data, 0x30) != 3)
            throw std::runtime_error("Unknown save summary");
        const auto descriptors = u32(data, 0x34);
        if (descriptors != 15 * 12 && descriptors != 16 * 12)
            throw std::runtime_error("Unknown save summary layout");
        const auto names_at = 0x38 + descriptors;
        const auto names_length = u32(data, names_at);
        if (names_length > 4096)
            throw std::runtime_error("Invalid summary names");
        const auto values_at = names_at + 4 + names_length;
        const auto values_length = u32(data, values_at);
        if (values_length > 4096)
            throw std::runtime_error("Invalid summary values");
        const auto table_at = values_at + 4 + values_length;
        const auto table_size = u32(data, table_at);
        const auto count = u32(data, table_at + 4);
        if (table_size < 8 || table_size > max_size || table_size > data.size() - table_at ||
            count == 0 || count > 512)
            throw std::runtime_error("Invalid save string table");
        const auto end = table_at + table_size;
        std::vector<std::string> strings;
        strings.reserve(count);
        auto position = table_at + 8;
        for (std::uint32_t index = 0; index < count; ++index)
        {
            if (position > end || end - position < 4)
                throw std::runtime_error("Truncated save string table");
            const auto length = u32(data, position);
            position += 4;
            if (length > 512 || length > end - position ||
                (length && data[position + length - 1] != 0))
                throw std::runtime_error("Invalid save string");
            for (std::size_t i = 0; i + 1 < length; ++i)
                if (data[position + i] == 0 || data[position + i] > 127)
                    throw std::runtime_error("Invalid save string encoding");
            strings.emplace_back(reinterpret_cast<const char *>(data.data() + position),
                                 length ? length - 1 : 0);
            position += length;
        }
        if (position != end || u32(data, 12) != 36)
            throw std::runtime_error("Unknown zone record layout");

        // as in SAVE_OPTIONS, type IDs depend on serialization order
        std::vector<std::pair<std::size_t, unsigned>> type_names;
        std::size_t names_end = end;
        for (const char *name : {"__internalObjectBase", "igObject", "igNamedObject", "igDirEntry", "igObjectDirEntry",
                                 "igMemoryDirEntry", "igExternalDirEntry", "igExternalIndexedEntry", "igExternalInfoEntry", "igExternalObjectEntry",
                                 "igContainer", "igDataList", "igObjectList", "igHashTable", "igStringBoolHashTable", "igStringIntHashTable",
                                 "igReferenceResolver", "igInfo", "igObjectDirectoryIGBInfo", "igNameList", "CGameSaveData", "igInfoList",
                                 "CDynamicDifficultySaveData", "CZoneInfoSaveList", "CZoneInfoSave", "CZoneInfoUserSession",
                                 "CGameVariableValueSavedBool", "CGameVariableValueSavedFloatList", "CGameVariableStoreSaveData",
                                 "CGameVariableValueSavedInt", "CGameVariableValueSavedIntList", "CGameVariableValueSavedBoolList",
                                 "CGameVariableValueSavedStringList", "CQuestSystemSaveData", "CGameVariableValueSavedFloat", "CQuestTrackSaveDataList"})
        {
            const std::string marker = std::string(name) + '\0';
            const auto found = std::search(data.begin() + end, data.end(), marker.begin(), marker.end());
            if (found == data.end())
                throw std::runtime_error("Unknown save types");
            type_names.emplace_back(found - data.begin(), type_names.size());
            names_end = std::max(names_end, found - data.begin() + marker.size());
        }
        std::sort(type_names.begin(), type_names.end());
        std::array<unsigned, 36> types{};
        for (unsigned i = 0; i < type_names.size(); ++i)
            types[type_names[i].second] = i;
        const auto directory_size = u32(data, 0), template_count = u32(data, 4);
        if (!template_count || template_count > 128 || directory_size > 4096 || directory_size < template_count * 20)
            throw std::runtime_error("Unknown save directory");
        Zones result;
        for (std::size_t start = names_end; start + directory_size + 12 < data.size(); ++start)
        {
            if (!((u32(data, start) == types[4] && u32(data, start + 4) == 20) ||
                  (u32(data, start) == types[5] && u32(data, start + 4) == 32)))
                continue;
            try
            {
                struct Entry
                {
                    unsigned type, count, width;
                    std::size_t at = 0;
                };
                std::vector<Entry> templates, entries;
                auto pos = start;
                for (unsigned i = 0; i < template_count; ++i)
                {
                    const auto kind = u32(data, pos), length = u32(data, pos + 4);
                    if (u32(data, pos + 8) != 0)
                        throw std::runtime_error("Invalid directory flags");
                    if (kind == types[4] && length == 20 && u32(data, pos + 12) > 0 && u32(data, pos + 12) < types.size())
                        templates.push_back({u32(data, pos + 12), 0, 0});
                    else if (kind == types[5] && length == 32 && u32(data, pos + 12) <= 4096 &&
                             (u32(data, pos + 24) == 1 || u32(data, pos + 24) == 4 || u32(data, pos + 24) == 8))
                    {
                        const auto field = u32(data, pos + 16);
                        // object and string references are 32-bit indices on disk.
                        templates.push_back({0, u32(data, pos + 12), field == 0 || field == 6 ? 4 : u32(data, pos + 24)});
                    }
                    else
                        throw std::runtime_error("Invalid directory entry");
                    pos += length;
                }
                if (pos != start + directory_size)
                    continue;
                const auto entry_count = u32(data, pos + 4);
                if (!entry_count || entry_count > 4096 || u32(data, pos) != 8 + entry_count * 2 ||
                    pos + 8 + entry_count * 2 + 4 > data.size())
                    continue;
                for (unsigned i = 0; i < entry_count; ++i)
                {
                    const auto at = pos + 8 + i * 2;
                    const unsigned index = data[at] | unsigned(data[at + 1]) << 8;
                    if (index >= templates.size())
                        throw std::runtime_error("Invalid directory reference");
                    entries.push_back(templates[index]);
                }
                pos += 8 + entry_count * 2;
                if (u32(data, pos) >= entry_count)
                    continue;
                pos += 4;
                std::size_t list = 0;
                for (auto &entry : entries)
                    if (entry.type)
                    {
                        const auto length = u32(data, pos + 4);
                        if (u32(data, pos) != entry.type || length < 8 || length > 4096 || length % 4 || length > data.size() - pos)
                            throw std::runtime_error("Invalid save object");
                        entry.at = pos;
                        if (entry.type == types[23])
                        {
                            if (list || length != 20)
                                throw std::runtime_error("Invalid zone list");
                            list = pos;
                        }
                        pos += length;
                    }
                for (auto &entry : entries)
                    if (!entry.type)
                    {
                        const auto length = entry.count * entry.width;
                        if (pos > data.size() || length > data.size() - pos)
                            throw std::runtime_error("Truncated save buffer");
                        entry.at = pos;
                        pos += length;
                    }
                if (pos != data.size() || !list)
                    continue;
                const auto zone_count = u32(data, list + 8), buffer = u32(data, list + 16);
                if (zone_count < 100 || zone_count > u32(data, list + 12) || buffer >= entry_count ||
                    entries[buffer].type || entries[buffer].width != 4 || entries[buffer].count != u32(data, list + 12))
                    throw std::runtime_error("Invalid zone list buffer");
                Zones current;
                for (unsigned i = 0; i < zone_count; ++i)
                {
                    const auto ref = u32(data, entries[buffer].at + i * 4);
                    if (ref >= entry_count || entries[ref].type != types[24] || u32(data, entries[ref].at + 4) != 16)
                        throw std::runtime_error("Invalid zone reference");
                    const auto zone = entries[ref].at;
                    const auto name = u32(data, zone + 8), session = u32(data, zone + 12);
                    if (name >= strings.size() || strings[name].find('/') == std::string::npos ||
                        session >= entry_count || entries[session].type != types[25] || u32(data, entries[session].at + 4) != 68)
                        throw std::runtime_error("Invalid zone session reference");
                    // a session can be serialized earlier in the graph, doesnt not have to follow its zone
                    const auto at = entries[session].at;
                    if (!current.emplace(strings[name], Zone{u32(data, at + 20), u32(data, at + 52), u32(data, at + 60)}).second)
                        throw std::runtime_error("Duplicate zone record");
                }
                if (!result.empty())
                    throw std::logic_error("Ambiguous zone records");
                result = std::move(current);
            }
            catch (const std::runtime_error &)
            {
                continue;
            }
        }
        if (result.empty())
            throw std::runtime_error("Incomplete zone records");
        return result;
    }

} // namespace save_probe
