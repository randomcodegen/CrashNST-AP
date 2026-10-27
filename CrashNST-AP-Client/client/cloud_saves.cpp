#include "cloud_saves.h"
#include "collectibles.h"
#include "Archipelago.h"
#include "standalone.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <set>
#include <stdexcept>

namespace
{
    constexpr wchar_t save_folder[] = L"Crash Bandicoot(TM) N. Sane Trilogy";
    constexpr unsigned max_file = 1024 * 1024;
    std::string json(const Json::Value &value)
    {
        Json::StreamWriterBuilder w;
        w["indentation"] = "";
        return Json::writeString(w, value);
    }
    bool same(const Json::Value &a, const Json::Value &b) { return json(a) == json(b); }
    std::string new_writer()
    {
        static std::atomic<unsigned> sequence{0};
        return state_digest(std::to_string(GetCurrentProcessId()) + ':' + std::to_string(GetTickCount64()) + ':' + std::to_string(++sequence));
    }
    void no_reparse(const std::filesystem::path &path)
    {
        for (auto p = std::filesystem::absolute(path); !p.empty();)
        {
            const auto attributes = GetFileAttributesW(p.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("AP save profile contains a link/junction. Refusing file access");
            const auto parent = p.parent_path();
            if (parent == p)
                break;
            p = parent;
        }
    }
    void write_binary(const std::filesystem::path &path, const std::vector<std::uint8_t> &bytes)
    {
        no_reparse(path);
        auto temporary = path;
        temporary += L".ap-tmp";
        no_reparse(temporary);
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot create AP display settings");
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                        written == bytes.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot persist AP display settings");
    }
    void inherit_display(const std::filesystem::path &source, const std::filesystem::path &saves, const std::filesystem::path &marker)
    {
        no_reparse(marker);
        if (std::filesystem::exists(marker))
            return;
        no_reparse(saves);
        std::filesystem::create_directories(saves);
        Json::Value inherited(Json::objectValue);
        for (const char *name : {"GraphicsOptions.sav", "WindowData.sav"})
        {
            const auto path = source / name;
            if (!std::filesystem::exists(path))
                continue;
            no_reparse(path);
            const auto bytes = save_probe::read_file(path);
            if (bytes.size() > max_file)
                throw std::runtime_error("Display preferences exceed their size bound");
            const auto target = saves / name;
            no_reparse(target);
            if (std::filesystem::exists(target))
            {
                auto backup = target;
                backup += L".before-inherit";
                no_reparse(backup);
                std::filesystem::copy_file(target, backup, std::filesystem::copy_options::skip_existing);
            }
            write_binary(target, bytes);
            inherited[name] = state_digest(std::string(bytes.begin(), bytes.end()));
        }
        write_state_json(marker, inherited);
    }
}

CloudSaves::CloudSaves(Session &session) : session_(session)
{
    root_ = session.profile_directory();
    saves_ = root_ / "AppData" / save_folder;
    key_ = session.storage_key();
    slot_hash_ = state_digest(json(session.slot_data()));
    writer_ = new_writer();
    no_reparse(root_);
    std::filesystem::create_directories(root_);
    const auto cache = root_ / "progress-cache.json";
    if (std::filesystem::exists(cache))
    {
        no_reparse(cache);
        const auto state = read_state_json(cache);
        if (session_.server_reset_pending())
        {
            write_state_json(root_ / ("progress-reset-" + writer_.substr(0, 16) + ".json"), state);
            return;
        }
        acknowledged_ = state["acknowledged"];
        pending_ = state["pending"];
        if (!acknowledged_.isNull())
            validate(acknowledged_);
        if (!pending_.isNull())
            validate(pending_);
        ready_ = !acknowledged_.isNull();
        if (ready_)
            session_.restore_cloud_progress(pending_.isNull() ? acknowledged_["progress"] : pending_["progress"]);
    }
}
Json::Value CloudSaves::initial() const
{
    Json::Value value(Json::objectValue);
    value["schema"] = 2;
    value["seed"] = session_.seed();
    value["team"] = session_.team();
    value["slot"] = session_.slot();
    value["slot_data_sha256"] = slot_hash_;
    value["generation"] = Json::UInt64(0);
    value["writer"] = "initial";
    value["epoch"] = writer_; // newly inserted default identifies a new server save
    value["progress"]["local_collectibles"] = Json::Value(Json::arrayValue);
    value["progress"]["filler_cursor"] = 0u;
    return value;
}
void CloudSaves::validate(const Json::Value &value) const
{
    if (!value.isObject() || value["schema"] != 2 || value["seed"] != session_.seed() || value["team"] != session_.team() ||
        value["slot"] != session_.slot() || value["slot_data_sha256"] != slot_hash_ ||
        !value["generation"].isUInt64() || value["generation"].asUInt64() > 1000000000 ||
        !value["writer"].isString() || value["writer"].asString().size() > 128 || value.isMember("files") ||
        !value["progress"].isObject() || value["progress"].size() != 2 ||
        json(value).size() > 16384)
        throw std::runtime_error("Cloud progress identity/schema/size does not match this AP slot");
    if (!value["epoch"].isString() || value["epoch"].asString().size() != 64)
        throw std::runtime_error("Invalid cloud progress epoch");
    const auto &local_collectibles = value["progress"]["local_collectibles"];
    if (!local_collectibles.isArray() || local_collectibles.size() > 4 * std::size(ingame::collectible_specs))
        throw std::runtime_error("Invalid cloud local collectible list");
    std::set<std::int64_t> local_collectible_ids;
    for (const auto &id : local_collectibles)
    {
        if (!id.isInt64() || !local_collectible_ids.insert(id.asInt64()).second)
            throw std::runtime_error("Invalid cloud local collectible identity");
        ingame::local_collectible_item(session_.slot_data(), id.asInt64());
    }
    const auto &cursor = value["progress"]["filler_cursor"];
    if (!cursor.isUInt() || cursor.asUInt() > 100000)
        throw std::runtime_error("Invalid cloud filler receipt cursor");
}
void CloudSaves::persist() const
{
    Json::Value cache(Json::objectValue);
    cache["acknowledged"] = acknowledged_;
    cache["pending"] = pending_;
    no_reparse(root_ / "progress-cache.json");
    write_state_json(root_ / "progress-cache.json", cache);
}
void CloudSaves::archive_local_saves()
{
    if (bound_)
        throw std::runtime_error("Close NST before resetting its local AP save profile");
    const auto appdata = root_ / "AppData";
    if (!std::filesystem::exists(appdata))
        return;
    const auto backup = root_ / "backups" / ("reset-" + writer_.substr(0, 16));
    no_reparse(appdata);
    no_reparse(backup);
    std::filesystem::create_directories(backup);
    std::filesystem::rename(appdata, backup / "AppData");
    const auto display = root_ / "display-inherited.json";
    no_reparse(display);
    if (std::filesystem::exists(display))
        std::filesystem::rename(display, backup / "display-inherited.json");
    std::cout << "Previous local AP game saves retained at " << backup.string() << '\n' << std::flush;
}
void CloudSaves::send(const Json::Value &value, const std::string &operation, const std::string &key)
{
    validate(value);
    std::string serialized = json(value), fallback = operation == "default" ? serialized : "null";
    AP_SetServerDataRequest request;
    request.key = key;
    request.type = AP_DataType::Raw;
    request.want_reply = true;
    request.default_value = &fallback;
    request.operations = {{operation, &serialized}};
    AP_SetServerData(&request);
}
void CloudSaves::connected()
{
    if (session_.server_reset_pending() && !acknowledged_.isNull())
    {
        if (bound_)
            throw std::runtime_error("Server save was reset. Close NST before restoring its authoritative state");
        Json::Value backup;
        backup["acknowledged"] = acknowledged_;
        backup["pending"] = pending_;
        write_state_json(root_ / ("progress-reset-" + writer_.substr(0, 16) + ".json"), backup);
        acknowledged_ = pending_ = Json::Value();
        ready_ = false;
    }
    writer_ = new_writer();
    handshake_ = true;
    sent_ = Json::Value();
    last_send_ = std::chrono::steady_clock::now();
    AP_SetNotify(key_, AP_DataType::Raw);
    send(initial(), "default", key_);
    std::cout << "AP progress requested. Gameplay waits for server progress.\n"
              << std::flush;
}
void CloudSaves::receive(const Json::Value &message)
{
    if (message["cmd"] != "SetReply" || message["key"] != key_)
        return;
    const auto &value = message["value"];
    validate(value);
    if (handshake_)
    {
        const bool active = bound_;
        const bool recreated = value["epoch"] == writer_ && value["generation"].asUInt64() == 0 &&
                               !acknowledged_.isNull() && !same(value, acknowledged_);
        if (recreated)
        {
            if (active)
                throw std::runtime_error("Server save was reset. Close NST and reconnect to restore its authoritative state");
            session_.reset_server_state();
            Json::Value backup;
            backup["acknowledged"] = acknowledged_;
            backup["pending"] = pending_;
            write_state_json(root_ / ("progress-reset-" + writer_.substr(0, 16) + ".json"), backup);
            acknowledged_ = value;
            pending_ = Json::Value();
            std::cout << "Server save was reset. Cached AP progress archived. Restoring server state.\n"
                      << std::flush;
        }
        else if (same(value, pending_))
        {
            acknowledged_ = value;
            pending_ = Json::Value();
        }
        else if (!acknowledged_.isNull() && !same(value, acknowledged_) && !pending_.isNull())
        {
            if (active || value["epoch"] != acknowledged_["epoch"] || value["epoch"] != pending_["epoch"] ||
                value["generation"].asUInt64() < acknowledged_["generation"].asUInt64())
            {
                write_state_json(root_ / "cloud-conflict.json", message);
                throw std::runtime_error("Server state changed while this profile has offline progress. Both copies were retained. Stop NST before resolving the cloud conflict");
            }
            Json::Value backup;
            backup["acknowledged"] = acknowledged_;
            backup["pending"] = pending_;
            backup["server"] = message;
            const auto path = root_ / ("progress-recovery-" + writer_.substr(0, 16) + ".json");
            no_reparse(path);
            write_state_json(path, backup);
            // same-epoch AP progress only grows: merge collectibles and keep the highest reserved filler index
            session_.restore_cloud_progress(pending_["progress"]);
            session_.restore_cloud_progress(value["progress"]);
            auto merged = value;
            merged["progress"] = session_.cloud_progress();
            if (same(merged["progress"], value["progress"]))
                merged = Json::Value();
            else
            {
                merged["generation"] = Json::UInt64(value["generation"].asUInt64() + 1);
                merged["writer"] = writer_;
                validate(merged);
            }
            acknowledged_ = value;
            pending_ = merged;
            std::cout << "Recovered offline AP progress. Both snapshots retained at " << path.string()
                      << ". NST save files unchanged.\n" << std::flush;
        }
        else if (!same(value, acknowledged_))
        {
            if (active)
                throw std::runtime_error("Close the running AP game before restoring a different server snapshot");
            acknowledged_ = value;
            pending_ = Json::Value();
        }
        session_.restore_cloud_progress(value["progress"]);
        ready_ = true;
        handshake_ = false;
        persist();
        if (session_.server_reset_pending())
        {
            archive_local_saves();
            session_.finish_server_reset();
        }
        std::cout << "AP progress ready (generation " << value["generation"].asUInt64() << "). NST save files stay on this computer.\n"
                  << "Local AP game save folder: " << saves_.string() << '\n'
                  << std::flush;
        return;
    }
    if (!sent_.isNull() && same(value, sent_))
    {
        const auto &original = message["original_value"];
        if (!same(original, acknowledged_) && !same(original, sent_))
        {
            write_state_json(root_ / "cloud-conflict.json", message);
            throw std::runtime_error("Another client wrote this AP slot concurrently. Stop NST. The competing snapshot is retained in cloud-conflict.json");
        }
        acknowledged_ = sent_;
        if (same(pending_, sent_))
            pending_ = Json::Value();
        sent_ = Json::Value();
        persist();
        std::cout << "AP progress acknowledged (generation " << acknowledged_["generation"].asUInt64() << ").\n"
                  << std::flush;
    }
    else if (!same(value, acknowledged_))
    {
        write_state_json(root_ / "cloud-conflict.json", message);
        throw std::runtime_error("Another client changed this AP slot's progress. Stop NST and reconnect. Local state was retained");
    }
}
DWORD CloudSaves::game_pid() const { return bound_ ? GetCurrentProcessId() : 0; }
void CloudSaves::adopt(const std::function<void(const std::filesystem::path &)> &bind)
{
    no_reparse(saves_);
    std::filesystem::create_directories(saves_);
    if ((saves_ / "SAVE_OPTIONS.sav").string().size() >= 250)
        throw std::runtime_error("AP profile path exceeds NST's save-path buffer");
    wchar_t appdata[32768]{};
    const auto count = GetEnvironmentVariableW(L"APPDATA", appdata, std::size(appdata));
    if (!count || count >= std::size(appdata))
        throw std::runtime_error("Cannot locate normal display preferences");
    inherit_display(std::filesystem::path(appdata) / save_folder, saves_, root_ / "display-inherited.json");
    bind(root_ / L"AppData");
    bound_ = true;
    std::cout << "Standalone AP save profile active. NST continues startup.\n"
              << std::flush;
}
void CloudSaves::tick(bool online, bool force)
{
    const auto now = std::chrono::steady_clock::now();
    if (handshake_ && online && now - last_send_ > std::chrono::seconds(10))
    {
        send(initial(), "default", key_);
        last_send_ = now;
    }
    if (!ready_ || (online && handshake_))
        return;
    if (!force && !bound_)
        adopt(standalone::bind_saves);
    if (!force && now < next_poll_)
        return;
    next_poll_ = now + std::chrono::seconds(1);
    auto value = acknowledged_;
    value["progress"] = session_.cloud_progress();
    if (same(value["progress"], acknowledged_["progress"]))
    {
        if (sent_.isNull() && !pending_.isNull())
        {
            pending_ = Json::Value();
            persist();
        }
    }
    else
    {
        value["generation"] = Json::UInt64(acknowledged_["generation"].asUInt64() + 1);
        value["writer"] = writer_;
        if (!same(value, pending_))
        {
            pending_ = value;
            persist();
        }
    }
    if (online && !handshake_ && sent_.isNull() && !pending_.isNull())
    {
        validate(pending_);
        // one writer per slot. SetNotify validates.
        send(acknowledged_, "replace", key_ + ":backup:previous");
        sent_ = pending_;
        send(sent_, "replace", key_);
        last_send_ = now;
    }
    else if (online && !handshake_ && !sent_.isNull() &&
             now - last_send_ > std::chrono::seconds(10))
        connected();
}
