#include "session.h"
#include "collectibles.h"
#include "seed_config.h"
#include <wincrypt.h>
#include <array>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace
{
    constexpr unsigned max_items = 100000, max_checks = 100000, max_bytes = 16 * 1024 * 1024;
    std::string digest(const std::string &bytes)
    {
        HCRYPTPROV provider = 0;
        HCRYPTHASH hash = 0;
        if (!CryptAcquireContext(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
            throw std::runtime_error("Cannot hash session identity");
        bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
                  CryptHashData(hash, reinterpret_cast<const BYTE *>(bytes.data()), static_cast<DWORD>(bytes.size()), 0);
        std::array<BYTE, 32> result{};
        DWORD size = 32;
        ok = ok && CryptGetHashParam(hash, HP_HASHVAL, result.data(), &size, 0) && size == 32;
        if (hash)
            CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        if (!ok)
            throw std::runtime_error("Cannot hash session identity");
        std::ostringstream text;
        for (auto byte : result)
            text << std::hex << std::setfill('0') << std::setw(2) << unsigned(byte);
        return text.str();
    }
    void atomic_json(const std::filesystem::path &path, const Json::Value &value)
    {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        const auto bytes = Json::writeString(writer, value);
        if (bytes.size() > max_bytes)
            throw std::runtime_error("Session exceeds size limit");
        auto temporary = path;
        temporary += L".tmp";
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create session replacement");
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                        written == bytes.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot persist session. No checks will be sent");
    }
    std::set<std::int64_t> locations(const Json::Value &array)
    {
        if (!array.isArray() || array.size() > max_checks)
            throw std::runtime_error("Invalid session location list");
        std::set<std::int64_t> result;
        for (const auto &value : array)
        {
            if (!value.isInt64() || value.asInt64() <= 0 || !result.insert(value.asInt64()).second)
                throw std::runtime_error("Invalid or duplicate session location");
        }
        return result;
    }
    Json::Value array(const std::set<std::int64_t> &values)
    {
        Json::Value result(Json::arrayValue);
        for (const auto value : values)
            result.append(Json::Int64(value));
        return result;
    }
    Json::Value read_json(const std::filesystem::path &path)
    {
        if (std::filesystem::file_size(path) > max_bytes)
            throw std::runtime_error("Session exceeds size limit");
        std::ifstream file(path, std::ios::binary);
        Json::CharReaderBuilder reader;
        std::string error;
        Json::Value result;
        if (!file || !Json::parseFromStream(reader, file, &result, &error) || !result.isObject())
            throw std::runtime_error("Invalid session JSON. Original file retained");
        return result;
    }
    bool save_name(const std::string &name)
    {
        if (name.size() < 10 || name.size() > 12 || name.compare(0, 5, "SAVE_") || name.substr(name.size() - 4) != ".sav")
            return false;
        for (auto i = 5u; i < name.size() - 4; ++i)
            if (name[i] < '0' || name[i] > '9')
                return false;
        return true;
    }
    void validate_item(const Json::Value &item)
    {
        if (!item.isObject() || !item["item"].isInt64() || item["item"].asInt64() <= 0 ||
            !item["location"].isInt64() || !item["player"].isInt() || item["player"].asInt() < 0 ||
            !item["flags"].isUInt())
            throw std::runtime_error("Invalid indexed AP item");
    }

}

std::string state_digest(const std::string &bytes) { return digest(bytes); }
Json::Value read_state_json(const std::filesystem::path &path) { return read_json(path); }
void write_state_json(const std::filesystem::path &path, const Json::Value &value) { atomic_json(path, value); }

Session::~Session()
{
    if (ready())
        CloseHandle(lease_);
}
bool Session::resume(const std::filesystem::path &directory, const std::string &profile, const std::string &expected_seed)
{
    const auto link = directory / ("profile-" + digest(profile) + ".json");
    if (!std::filesystem::exists(link))
        return false;
    const auto identity = read_json(link);
    if (identity["schema"] != 1 || identity["profile"] != profile || !identity["seed"].isString() ||
        !identity["team"].isInt() || !identity["slot"].isInt())
        throw std::runtime_error("Invalid saved session profile");
    const auto seed = identity["seed"].asString();
    const auto team = identity["team"].asInt(), slot = identity["slot"].asInt();
    if (!expected_seed.empty() && seed != expected_seed)
        return false;
    const auto state = read_json(directory / (digest(seed + '\n' + std::to_string(team) + '\n' + std::to_string(slot)) + ".json"));
    // reject unusable cached rules before acquiring or rewriting any session state
    const SeedConfig validated(state["slot_data"]);
    open(directory, seed, team, slot, state["slot_data"]);
    return true;
}
void Session::remember(const std::filesystem::path &directory, const std::string &profile)
{
    if (!ready())
        throw std::runtime_error("Cannot remember an unidentified session");
    Json::Value identity(Json::objectValue);
    identity["schema"] = 1;
    identity["profile"] = profile;
    for (const auto key : {"seed", "team", "slot"})
        identity[key] = data_[key];
    atomic_json(directory / ("profile-" + digest(profile) + ".json"), identity);
}
void Session::open(const std::filesystem::path &directory, const std::string &seed,
                   int team, int slot, const Json::Value &slot_data)
{
    if (seed.empty() || seed.size() > 256 || team < 0 || slot <= 0 || !slot_data.isObject())
        throw std::runtime_error("Invalid AP session identity");
    if (slot_data["protocol"] != seed_protocol || !slot_data["checks"].isObject() || slot_data["checks"].empty() ||
        !slot_data["check_requirements"].isObject() || slot_data["check_requirements"].size() != slot_data["checks"].size() ||
        !slot_data["level_placements"].isObject() || !slot_data["victory_goal"].isObject() ||
        !slot_data["collectible_locations"].isObject() ||
        !slot_data["local_reward_sources"].isObject() || !slot_data["box_item_spoilers"].isBool() ||
        !slot_data["death_link"].isBool())
        throw std::runtime_error("Unsupported NST slot data. Generate a seed with the current APWorld");
    if (ready() && data_["seed"] == seed && data_["team"] == team && data_["slot"] == slot)
    {
        if (data_["slot_data"] != slot_data)
            throw std::runtime_error("Seed slot data changed. Refusing old session state");
        return;
    }
    if (ready())
    {
        CloseHandle(lease_);
        lease_ = INVALID_HANDLE_VALUE;
    }
    data_ = Json::Value();
    std::filesystem::create_directories(directory);
    path_ = directory / (digest(seed + '\n' + std::to_string(team) + '\n' + std::to_string(slot)) + ".json");
    auto lock = path_;
    lock += L".lock";
    lease_ = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!ready())
        throw std::runtime_error("Session already in use, or state directory is not writable");
    try
    {
        if (std::filesystem::exists(path_))
        {
            data_ = read_json(path_);
            if (data_["schema"] != 1 || data_["seed"] != seed || data_["team"] != team || data_["slot"] != slot ||
                data_["slot_data"] != slot_data || !data_["bindings"].isObject() ||
                !data_["items"].isArray() || data_["items"].size() > max_items)
                throw std::runtime_error("Session identity, schema, or slot data does not match");
            locations(data_["pending"]);
            locations(data_["checked"]);
            if (!data_.isMember("local_clears"))
            {
                data_["local_clears"] = Json::Value(Json::arrayValue);
                // older clients retained local provenance only while a check was pending
                for (auto id : pending())
                    if (data_["slot_data"]["checks"][std::to_string(id)]["event"] == "clear")
                        data_["local_clears"].append(Json::Int64(id));
            }
            local_clears();
            local_collectibles();
            hub_entrances();
            if (!location_flags().isObject() || location_flags().size() > max_checks)
                throw std::runtime_error("Invalid saved location flags");
            for (const auto &key : location_flags().getMemberNames())
                if (!slot_data["checks"].isMember(key) || !location_flags()[key].isUInt() || location_flags()[key].asUInt() > 7)
                    throw std::runtime_error("Invalid saved location flags");
            for (const auto &item : data_["items"])
                validate_item(item);
            if (!data_["filler_cursor"].isUInt() || filler_cursor() > max_items)
                throw std::runtime_error("Invalid saved filler cursor");
            if (!data_["server_reset_pending"].isBool())
                throw std::runtime_error("Invalid server reset marker");
            for (const auto &key : data_["bindings"].getMemberNames())
            {
                const auto &value = data_["bindings"][key];
                if (key.size() != 1 || key[0] < '1' || key[0] > '3' || !value.isString() ||
                    !save_name(value.asString()))
                    throw std::runtime_error("Invalid session save binding");
            }
        }
        else
        {
            data_["schema"] = 1;
            data_["seed"] = seed;
            data_["team"] = team;
            data_["slot"] = slot;
            data_["slot_data"] = slot_data;
            data_["bindings"] = Json::Value(Json::objectValue);
            data_["location_flags"] = Json::Value(Json::objectValue);
            data_["local_collectibles"] = Json::Value(Json::arrayValue);
            data_["filler_cursor"] = 0u;
            data_["server_reset_pending"] = false;
            data_["pending"] = data_["checked"] = data_["items"] = Json::Value(Json::arrayValue);
            data_["local_clears"] = Json::Value(Json::arrayValue);
        }
        dirty_ = true;
        flush();
    }
    catch (...)
    {
        CloseHandle(lease_);
        lease_ = INVALID_HANDLE_VALUE;
        data_ = Json::Value();
        throw;
    }
}
bool Session::received(unsigned index, const Json::Value &incoming)
{
    if (!ready())
        throw std::runtime_error("Received items before session identity");
    if (!incoming.isArray() || index > max_items || incoming.size() > max_items - index)
        throw std::runtime_error("Invalid AP item list size");
    for (const auto &item : incoming)
        validate_item(item);
    if (index > data_["items"].size())
        return false;
    auto replacement = data_["items"];
    for (unsigned i = 0; i < incoming.size(); ++i)
    {
        Json::Value item(Json::objectValue);
        for (const auto key : {"item", "location", "player", "flags"})
            item[key] = incoming[i][key];
        if (index + i < replacement.size())
        {
            const auto &previous = replacement[index + i];
            if (previous["item"].asInt64() != item["item"].asInt64() || previous["location"].asInt64() != item["location"].asInt64() ||
                previous["player"].asInt() != item["player"].asInt() || previous["flags"].asUInt() != item["flags"].asUInt())
                throw std::runtime_error("AP changed a received item at an existing index");
            continue;
        }
        replacement[index + i] = item;
    }
    if (replacement != data_["items"])
    {
        data_["items"] = std::move(replacement);
        dirty_ = true;
    }
    return true;
}
std::set<std::int64_t> Session::scout_locations() const
{
    std::set<std::int64_t> result;
    if (!ready() || slot_data()["box_item_spoilers"] != true)
        return result;
    const auto &checks = slot_data()["checks"];
    if (!checks.isObject() || checks.size() > max_checks)
        throw std::runtime_error("Invalid scout location list");
    for (const auto &key : checks.getMemberNames())
    {
        const auto id = std::stoll(key);
        if (id <= 0 || std::to_string(id) != key)
            throw std::runtime_error("Invalid scout location ID");
        result.insert(id);
    }
    return result;
}
void Session::scouted(const Json::Value &incoming)
{
    if (!ready() || slot_data()["box_item_spoilers"] != true)
        return;
    if (!incoming.isArray() || incoming.size() > max_checks)
        throw std::runtime_error("Invalid scouted location list");
    auto flags = location_flags();
    for (const auto &item : incoming)
    {
        if (!item["location"].isInt64() || item["location"].asInt64() <= 0 ||
            !item["flags"].isUInt() || item["flags"].asUInt() > 7)
            throw std::runtime_error("Invalid scouted location flags");
        const auto key = std::to_string(item["location"].asInt64());
        if (!slot_data()["checks"].isMember(key))
            throw std::runtime_error("Scouted location is absent from this AP room");
        flags[key] = item["flags"];
    }
    if (flags != location_flags())
    {
        data_["location_flags"] = std::move(flags);
        dirty_ = true;
    }
}
void Session::acknowledge(const Json::Value &incoming)
{
    auto acknowledged = checked(), queued = pending();
    for (const auto value : locations(incoming))
    {
        acknowledged.insert(value);
        queued.erase(value);
    }
    if (acknowledged.size() > max_checks)
        throw std::runtime_error("Too many acknowledged checks");
    if (array(acknowledged) != data_["checked"] || array(queued) != data_["pending"])
    {
        data_["checked"] = array(acknowledged);
        data_["pending"] = array(queued);
        dirty_ = true;
    }
}
void Session::connected(const Json::Value &incoming, bool can_reset)
{
    if (!ready())
        throw std::runtime_error("Connected checks before session identity");
    const auto authoritative = locations(incoming), old = checked();
    const bool rollback = std::any_of(old.begin(), old.end(), [&](auto id)
                                      { return !authoritative.count(id); });
    if (rollback)
    {
        if (!can_reset)
            throw std::runtime_error("Server progress was reset. Close NST and reconnect to restore its authoritative state");
        reset_server_state();
    }
    auto queued = pending();
    for (auto id : authoritative)
        queued.erase(id);
    data_["checked"] = array(authoritative);
    data_["pending"] = array(queued);
    dirty_ = true;
    flush();
}
void Session::reset_server_state()
{
    if (!ready())
        throw std::runtime_error("Server reset before session identity");
    if (!server_reset_pending())
    {
        auto backup = path_;
        backup += L".before-server-reset-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        atomic_json(backup, data_);
    }
    data_["items"] = data_["pending"] = Json::Value(Json::arrayValue);
    data_["local_collectibles"] = Json::Value(Json::arrayValue);
    data_["local_clears"] = Json::Value(Json::arrayValue);
    data_["bindings"] = Json::Value(Json::objectValue);
    data_.removeMember("hub_entrances");
    data_["filler_cursor"] = 0u;
    data_["server_reset_pending"] = true;
    dirty_ = true;
    flush();
}
void Session::finish_server_reset()
{
    data_["server_reset_pending"] = false;
    dirty_ = true;
    flush();
}
std::set<std::int64_t> Session::queue(const std::set<std::int64_t> &incoming)
{
    if (!ready())
        throw std::runtime_error("Invalid queued session");
    const auto acknowledged = checked();
    auto values = pending();
    const auto old_clears = local_clears();
    auto clears = old_clears;
    std::set<std::int64_t> added;
    for (auto location : incoming)
    {
        if (location <= 0 || !data_["slot_data"]["checks"].isMember(std::to_string(location)))
            throw std::runtime_error("Queued location is absent from this AP room");
        if (data_["slot_data"]["checks"][std::to_string(location)]["event"] == "clear")
            clears.insert(location);
        if (!acknowledged.count(location) && values.insert(location).second)
            added.insert(location);
    }
    if (values.size() > max_checks)
        throw std::runtime_error("Too many pending checks");
    if (added.empty() && clears == old_clears)
        return added;
    auto previous = data_["pending"];
    data_["pending"] = array(values);
    data_["local_clears"] = array(clears);
    dirty_ = true;
    try
    {
        flush(); // persist the entire batch before callers send any check
    }
    catch (...)
    {
        data_["pending"] = std::move(previous);
        data_["local_clears"] = array(old_clears);
        dirty_ = true;
        throw;
    }
    return added;
}
std::map<char, std::string> Session::hub_entrances() const
{
    std::map<char, std::string> result;
    if (!data_.isMember("hub_entrances"))
        return result;
    const auto &saved = data_["hub_entrances"];
    if (!saved.isObject() || saved.size() > 3)
        throw std::runtime_error("Invalid saved hub entrances");
    for (const auto &game : saved.getMemberNames())
    {
        const auto &value = saved[game];
        const auto entrance = value.isString() ? value.asString() : std::string();
        if (game.size() != 1 || game[0] < '1' || game[0] > '3' || entrance.size() != 4 ||
            (entrance[0] != 'L' && entrance[0] != 'B') || entrance[1] != game[0] ||
            !data_["slot_data"]["level_placements"].isMember(entrance))
            throw std::runtime_error("Invalid saved hub entrance");
        result.emplace(game[0], entrance);
    }
    return result;
}
void Session::remember_entrance(const std::string &entrance)
{
    if (!ready() || entrance.size() != 4 || entrance[1] < '1' || entrance[1] > '3' ||
        (entrance[0] != 'L' && entrance[0] != 'B') || !data_["slot_data"]["level_placements"].isMember(entrance))
        throw std::runtime_error("Cannot remember an entrance outside this AP room");
    const std::string game(1, entrance[1]);
    const auto previous = data_.get("hub_entrances", Json::Value(Json::objectValue));
    if (previous[game] == entrance)
        return;
    data_["hub_entrances"][game] = entrance;
    dirty_ = true;
    try { flush(); }
    catch (...)
    {
        data_["hub_entrances"] = previous;
        dirty_ = true;
        throw;
    }
}
void Session::flush()
{
    if (ready() && dirty_)
    {
        atomic_json(path_, data_);
        dirty_ = false;
    }
}
std::set<std::int64_t> Session::local_collectibles() const
{
    if (!ready())
        return {};
    const auto result = locations(data_["local_collectibles"]);
    for (auto id : result)
        ingame::local_collectible_item(data_["slot_data"], id);
    return result;
}
void Session::observe_collectible(std::int64_t location)
{
    if (!ready())
        throw std::runtime_error("Local collectible before session identity");
    ingame::local_collectible_item(data_["slot_data"], location);
    auto observed = local_collectibles();
    if (observed.insert(location).second)
    {
        data_["local_collectibles"] = array(observed);
        dirty_ = true;
        flush();
    }
}
std::set<std::int64_t> Session::pending() const { return ready() ? locations(data_["pending"]) : std::set<std::int64_t>{}; }
std::set<std::int64_t> Session::checked() const { return ready() ? locations(data_["checked"]) : std::set<std::int64_t>{}; }
std::set<std::int64_t> Session::local_clears() const
{
    if (!ready())
        return {};
    const auto result = locations(data_["local_clears"]);
    for (auto id : result)
        if (data_["slot_data"]["checks"][std::to_string(id)]["event"] != "clear")
            throw std::runtime_error("Invalid locally earned clear");
    return result;
}
std::vector<std::pair<std::int64_t, int>> Session::items() const
{
    std::vector<std::pair<std::int64_t, int>> result;
    if (ready())
        for (const auto &item : data_["items"])
            result.emplace_back(item["item"].asInt64(), item["player"].asInt());
    return result;
}
bool Session::reserve_filler(unsigned index)
{
    if (!ready() || index >= data_["items"].size())
        throw std::runtime_error("Invalid filler receipt index");
    if (index < filler_cursor())
        return false;
    const auto item = data_["items"][index]["item"].asInt64();
    if (item != 0xCB0000 + 2201 && item != 0xCB0000 + 2202)
        throw std::runtime_error("Receipt is not a filler reward");
    const auto old = filler_cursor();
    data_["filler_cursor"] = index + 1;
    dirty_ = true;
    try
    {
        flush();
    }
    catch (...)
    {
        data_["filler_cursor"] = old;
        dirty_ = true;
        throw;
    }
    return true;
}
std::map<char, std::string> Session::bind_saves(const std::filesystem::path &save_directory,
                                                const std::set<std::string> &levels)
{
    if (!ready())
        return {};
    std::map<char, std::string> result;
    for (const auto &key : data_["bindings"].getMemberNames())
        result[key[0]] = data_["bindings"][key].asString();
    const auto options = save_directory / "SAVE_OPTIONS.sav";
    const auto recent = std::filesystem::exists(options) ? save_probe::recent_slots(save_probe::read_file(options)) : std::map<char, std::string>{};
    for (char campaign = '1'; campaign <= '3'; ++campaign)
    {
        if (std::none_of(levels.begin(), levels.end(), [&](const auto &level)
                         { return level[1] == campaign; }) ||
            !recent.count(campaign) || !std::filesystem::exists(save_directory / recent.at(campaign)))
            continue;
        const auto &chosen = recent.at(campaign);
        if (!result.count(campaign))
            save_probe::zones(save_probe::read_file(save_directory / chosen));
        result[campaign] = chosen;
    }
    for (const auto &[campaign, file] : result)
    {
        const std::string key(1, campaign);
        if (data_["bindings"][key] != file)
        {
            data_["bindings"][key] = file;
            dirty_ = true;
        }
    }
    flush();
    return result;
}
Json::Value Session::cloud_progress() const
{
    Json::Value progress(Json::objectValue);
    progress["filler_cursor"] = filler_cursor();
    progress["local_collectibles"] = array(local_collectibles());
    return progress;
}
void Session::restore_cloud_progress(const Json::Value &progress)
{
    if (!ready() || !progress.isObject() || progress.size() != 2)
        throw std::runtime_error("Invalid cloud AP progress");
    const auto &cursor = progress["filler_cursor"];
    if (!cursor.isUInt() || cursor.asUInt() > max_items)
        throw std::runtime_error("Invalid cloud filler cursor");
    auto collected = local_collectibles();
    for (auto id : locations(progress["local_collectibles"]))
    {
        ingame::local_collectible_item(data_["slot_data"], id);
        collected.insert(id);
    }
    data_["filler_cursor"] = std::max(filler_cursor(), cursor.asUInt());
    data_["local_collectibles"] = array(collected);
    dirty_ = true;
    flush();
}
