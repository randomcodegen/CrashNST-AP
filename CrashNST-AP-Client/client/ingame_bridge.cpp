#include "ingame_bridge.h"
#include "game_memory.h"
#include "game_hooks.h"
#include "console.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
extern "C" DWORD WINAPI NST_Start(void *);
extern "C" void NST_Tick();
extern "C" unsigned NST_MovementCampaign(std::uintptr_t);
extern "C" unsigned NST_DoubleJumpAllowed(unsigned);
extern "C" std::uintptr_t NST_CrateIdentity(std::uintptr_t, const char **);

BoxMarkers::BoxMarkers(DWORD pid, bool startup, unsigned marker_mode) : startup_(startup), pid_(pid)
{
    if (pid != GetCurrentProcessId())
        throw std::invalid_argument("In-game bridge requires the current NST process");
    if (marker_mode > ingame::MarkersTint)
        throw std::invalid_argument("Invalid crate marker mode");
    try
    {
        mutex_ = CreateMutexW(nullptr, FALSE, ingame::name(pid, L"_mutex").c_str());
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ingame::Shared), ingame::name(pid, L"_state").c_str());
        if (!mutex_ || !mapping_)
            throw std::runtime_error("Cannot create in-game bridge");
        shared_ = static_cast<ingame::Shared *>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ingame::Shared)));
        if (!shared_)
            throw std::runtime_error("Cannot map in-game bridge");
        lock();
        if (shared_->client_pid && shared_->client_pid != GetCurrentProcessId() && !shared_->stop)
        {
            const auto owner = OpenProcess(SYNCHRONIZE, FALSE, shared_->client_pid);
            const bool alive = owner && WaitForSingleObject(owner, 0) == WAIT_TIMEOUT;
            if (owner)
                CloseHandle(owner);
            if (alive)
            {
                unlock();
                throw std::runtime_error("Another AP client owns this game");
            }
        }
        if (shared_->running)
        {
            shared_->stop = 1;
            unlock();
            for (unsigned i = 0; i < 100; ++i)
            {
                Sleep(10);
                lock();
                const auto busy = shared_->running;
                unlock();
                if (!busy)
                    break;
            }
            lock();
            if (shared_->running)
            {
                unlock();
                throw std::runtime_error("Old in-game adapter is still restoring. Restart NST");
            }
        }
        // existing disabled DLLs remain . reinitialize only while holding their mutex.
        std::memset(shared_, 0, sizeof(*shared_));
        shared_->version = ingame::protocol;
        shared_->bytes = sizeof(*shared_);
        shared_->box_half_size = 50;
        shared_->hub_label_distances = ingame::default_hub_distances;
        shared_->tint_opacity = ingame::default_tint_opacity;
        if (const auto size = std::getenv("CRASH_NST_MARKER_SIZE"))
        {
            char *end = nullptr;
            const auto number = std::strtof(size, &end);
            if (end != size && !*end && std::isfinite(number) && number >= 10 && number <= 250)
                shared_->box_half_size = number;
        }
        shared_->marker_mode = marker_mode;
        shared_->visible = 1;
        shared_->client_pid = GetCurrentProcessId();
        owned_ = true;
        unlock();
        const auto process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
        if (!process)
            throw std::runtime_error("Cannot open NST for in-game adapter");
        try
        {
            if (!NST_Start(startup ? reinterpret_cast<void *>(1) : nullptr))
                throw std::runtime_error("Standalone renderer startup is not ready");
            tick_ = reinterpret_cast<std::uintptr_t>(NST_Tick);
            double_jump_filter_ = reinterpret_cast<std::uintptr_t>(NST_DoubleJumpAllowed);
            crate_identity_ = reinterpret_cast<std::uintptr_t>(NST_CrateIdentity);
            // startup only renders the AP console. NST's code/data cant be unpacked yet
            // gameplay hooks are left for later
            if (!startup)
            {
                // redirect this movement campaign query only. the game's other campaign queries stay unchanged.
                const auto site = nst_module_base(process) + 0x4a9190;
                // a nearby relay bridges the five-byte call's +/-2 GiB range to an arbitrary DLL address.
                const auto aligned = site & ~std::uintptr_t(0xffff);
                std::uintptr_t relay = 0;
                for (std::uintptr_t distance = 0x10000; !relay && distance < 0x7fff0000; distance += 0x10000)
                    for (const auto address : {aligned + distance, aligned > distance ? aligned - distance : 0})
                    {
                        MEMORY_BASIC_INFORMATION region{};
                        if (!address || !VirtualQueryEx(process, reinterpret_cast<void *>(address), &region, sizeof(region)) || region.State != MEM_FREE || region.RegionSize < 4096)
                            continue;
                        relay = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process, reinterpret_cast<void *>(address), 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                        if (relay)
                            break;
                    }
                if (!relay)
                    throw std::runtime_error("Cannot allocate movement-query relay");
                // mov rax, callback. jmp rax preserves the original caller's return address and arguments.
                std::array<unsigned char, 12> code{{0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xe0}};
                const auto query = reinterpret_cast<std::uintptr_t>(NST_MovementCampaign);
                std::memcpy(code.data() + 2, &query, 8);
                SIZE_T written = 0;
                DWORD old = 0;
                if (!WriteProcessMemory(process, reinterpret_cast<void *>(relay), code.data(), code.size(), &written) || written != code.size() ||
                    !VirtualProtectEx(process, reinterpret_cast<void *>(relay), 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(process, reinterpret_cast<void *>(relay), code.size()))
                    throw std::runtime_error("Cannot finalize movement-query relay");
                movement_call_ = ability_jump_call(site, relay);
                patch_relative_call(process, pid, site, {{0xe8, 0x6b, 0x82, 0xc8, 0xff}}, movement_call_);
                movement_site_ = site;
            }
            CloseHandle(process);
        }
        catch (...)
        {
            CloseHandle(process);
            throw;
        }
    }
    catch (...)
    {
        if (shared_)
        {
            if (owned_)
            {
                try
                {
                    lock();
                    shared_->stop = 1;
                    unlock();
                }
                catch (...)
                {
                }
            }
            UnmapViewOfFile(shared_);
            shared_ = nullptr;
        }
        if (mapping_)
            CloseHandle(mapping_);
        if (mutex_)
            CloseHandle(mutex_);
        throw;
    }
}
void BoxMarkers::lock()
{
    const auto result = WaitForSingleObject(mutex_, 1000);
    if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
        throw std::runtime_error("In-game bridge is busy");
}
void BoxMarkers::unlock()
{
    shared_->heartbeat = GetTickCount64();
    ReleaseMutex(mutex_);
}
BoxMarkers::~BoxMarkers()
{
    try
    {
        lock();
        shared_->stop = 1;
        unlock();
    }
    catch (...)
    {
    }
    if (movement_site_)
    {
        const auto process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid_);
        if (process)
        {
            try
            {
                patch_relative_call(process, pid_, movement_site_, movement_call_, {{0xe8, 0x6b, 0x82, 0xc8, 0xff}});
            }
            catch (const std::exception &e)
            {
                std::cerr << "Movement query restoration: " << e.what() << ". Restart NST.\n";
            }
            CloseHandle(process);
        }
    }
    if (shared_)
        UnmapViewOfFile(shared_);
    if (mapping_)
        CloseHandle(mapping_);
    if (mutex_)
        CloseHandle(mutex_);
}
void BoxMarkers::update(std::vector<BoxMarker> markers)
{
    if (markers.size() > ingame::max_boxes)
        throw std::runtime_error("Too many selected box markers");
    lock();
    shared_->box_count = static_cast<unsigned>(markers.size());
    for (unsigned i = 0; i < markers.size(); ++i)
    {
        auto &box = shared_->boxes[i];
        ingame::copy(box.level, markers[i].level);
        ingame::copy(box.name, markers[i].name);
        std::memcpy(box.position, markers[i].position.data(), 12);
        box.color = markers[i].color;
    }
    ++shared_->revision;
    unlock();
}
void BoxMarkers::message(std::string text)
{
    lock();
    ingame::copy(shared_->status, text);
    ++shared_->revision;
    unlock();
}
void BoxMarkers::goal(const std::string &text)
{
    lock();
    ingame::copy(shared_->goal_counts, text);
    ++shared_->revision;
    unlock();
}
void BoxMarkers::connection(const std::string &text)
{
    lock();
    ingame::copy(shared_->binding, text);
    ++shared_->revision;
    unlock();
}
void BoxMarkers::startup_popup(bool show)
{
    lock();
    shared_->startup_popup = show;
    unlock();
}
void BoxMarkers::c3_buttons(bool ready, const std::set<std::string> &allowed)
{
    std::uint64_t mask = 0;
    const auto keys = c3_button_keys();
    if (ready)
        for (unsigned i = 0; i < keys.size(); ++i)
            if (allowed.count(keys[i]))
                mask |= std::uint64_t(1) << i;
    lock();
    shared_->c3_buttons_ready = ready;
    shared_->c3_buttons_active = mask;
    unlock();
}
void BoxMarkers::progress(const CheckProgress &counts, const std::map<std::string, std::string> &placements,
                          const std::set<std::string> &unlocked)
{
    if (counts.levels.size() > ingame::max_levels)
        throw std::runtime_error("Too many level labels");
    lock();
    shared_->level_count = 0;
    const auto campaigns = campaign_progress(counts, placements);
    for (unsigned i = 0; i < campaigns.size(); ++i)
        ingame::copy(shared_->campaign_counts[i], campaigns[i].text());
    for (const auto &[key, p] : counts.levels)
    {
        auto &level = shared_->levels[shared_->level_count++];
        ingame::copy(level.key, key);
        ingame::copy(level.title, p.title);
        ingame::copy(level.counts, p.text());
        ingame::copy(level.collectible_counts, p.collectible_text());
    }
    shared_->entrance_count = 0;
    std::set<char> hubs;
    for (const auto &[source, destination] : placements)
        if (std::none_of(secret_stages.begin(), secret_stages.end(),
                         [&](const auto &entry) { return source == entry.first; }))
            hubs.insert(source[1]);
    int c3_index = 0;
    for (const auto &anchor : entrance_anchors())
    {
        const auto index = anchor.hub == "L300" ? c3_index++ : -1;
        if (!hubs.count(anchor.hub[1]))
            continue;
        const bool secret = anchor.level[0] == 'S';
        const auto found = placements.find(anchor.level);
        if (!secret && found == placements.end())
            continue;
        const auto destination = secret ? "L" + anchor.level.substr(1) : found == placements.end() ? anchor.level : found->second;
        if (!unlocked.count(secret ? anchor.level : destination))
            continue;
        auto &entry = shared_->entrances[shared_->entrance_count++];
        if (index < 0 || entry.c3_button != index || std::strcmp(entry.hub, anchor.hub.c_str()))
            std::memcpy(entry.position, anchor.position.data(), 12);
        entry.c3_button = index;
        ingame::copy(entry.hub, anchor.hub);
        ingame::copy(entry.destination, destination);
        ingame::copy(entry.source, anchor.level);
    }
    ++shared_->revision;
    unlock();
}
void BoxMarkers::configure(const std::map<std::string, unsigned> &styles, bool death_link)
{
    lock();
    shared_->death_link = death_link;
    for (unsigned i = 0; i < shared_->level_count; ++i)
    {
        const auto found = styles.find(shared_->levels[i].key);
        shared_->levels[i].style = found == styles.end() ? 0 : found->second;
    }
    ++shared_->revision;
    unlock();
}
unsigned BoxMarkers::collectibles(std::vector<ingame::Collectible> config)
{
    if (config.size() > ingame::max_levels)
        throw std::runtime_error("Too many collectible levels");
    for (const auto &c : config)
    {
        const auto spec = ingame::collectible_spec(c.key);
        if (!spec || ((c.owned | c.selected | c.blocked | c.checked) & ~ingame::collectible_bits(*spec)) ||
            ((c.blocked | c.checked) & ~c.selected))
            throw std::runtime_error("Invalid local collectible config");
    }
    lock();
    shared_->collectible_count = static_cast<unsigned>(config.size());
    std::copy(config.begin(), config.end(), shared_->collectibles);
    const auto awards = shared_->collectible_awards;
    ++shared_->revision;
    unlock();
    return awards;
}
std::vector<ingame::CollectibleObservation> BoxMarkers::collectible_observations()
{
    lock();
    const bool ok = ingame::valid(*shared_);
    std::vector<ingame::CollectibleObservation> result;
    if (ok)
        result.assign(shared_->collectible_observations, shared_->collectible_observations + shared_->collectible_observation_count);
    unlock();
    if (!ok)
        throw std::runtime_error("Invalid local collectible observations");
    return result;
}
void BoxMarkers::feed(ingame::Line line, bool popup)
{
    line.count = std::min(line.count, ingame::max_spans);
    line.ticks = popup ? GetTickCount64() : 0;
    lock();
    ingame::append_console_line(*shared_, line);
    unlock();
}
void BoxMarkers::completion(const std::vector<std::string> &items, const std::vector<std::string> &locations)
{
    lock();
    unsigned used = 0;
    const auto append = [&](const std::vector<std::string> &names)
    {for(const auto& name:names){
        if(name.size()>1023 || used+name.size()+1>sizeof(shared_->completion))continue;
        std::memcpy(shared_->completion+used,name.c_str(),name.size()+1);used+=static_cast<unsigned>(name.size()+1);} };
    shared_->item_start = 0;
    append(items);
    shared_->location_start = used;
    append(locations);
    shared_->completion_bytes = used;
    ++shared_->revision;
    unlock();
}
std::vector<std::string> BoxMarkers::commands()
{
    std::vector<std::string> result;
    lock();
    if (!ingame::valid(*shared_))
    {
        unlock();
        throw std::runtime_error("Invalid in-game bridge counters");
    }
    while (shared_->command_read != shared_->command_write)
    {
        const auto &text = shared_->commands[shared_->command_read++ % ingame::max_commands];
        if (std::memchr(text, 0, sizeof(text)))
            result.emplace_back(text);
    }
    const std::string error = shared_->error;
    shared_->error[0] = 0;
    unlock();
    if (!error.empty())
        std::cerr << "In-game adapter: " << error << std::endl;
    return result;
}
void BoxMarkers::receive_death()
{
    lock();
    ++shared_->death_request;
    unlock();
}
unsigned BoxMarkers::deaths()
{
    lock();
    const auto value = shared_->deaths;
    unlock();
    return value;
}
bool BoxMarkers::rendering()
{
    lock();
    const bool value = shared_->attached;
    unlock();
    return value;
}
void BoxMarkers::marker_mode(unsigned mode)
{
    if (mode > ingame::MarkersTint)
        throw std::invalid_argument("Invalid crate marker mode");
    lock();
    shared_->marker_mode = mode;
    ++shared_->revision;
    unlock();
}
void BoxMarkers::hub_distance(const std::array<unsigned, 3> &distances)
{
    if (std::any_of(distances.begin(), distances.end(), [](unsigned distance) { return distance > ingame::max_hub_distance; }))
        throw std::invalid_argument("Invalid hub label distance");
    lock();
    shared_->hub_label_distances = distances;
    ++shared_->revision;
    unlock();
}
void BoxMarkers::tint_opacity(float opacity)
{
    if (!ingame::valid_tint_opacity(opacity))
        throw std::invalid_argument("Invalid tint opacity");
    lock();
    shared_->tint_opacity = opacity;
    ++shared_->revision;
    unlock();
}
bool BoxMarkers::toggle(const std::string &target, const std::string &value)
{
    if (value != "toggle" && value != "on" && value != "off")
        throw std::invalid_argument("Use on, off or toggle");
    lock();
    auto &flag = target == "markers" ? shared_->visible : shared_->vanilla_controls;
    bool enabled = target == "markers" ? flag : !flag;
    enabled = value == "toggle" ? !enabled : value == "on";
    flag = target == "markers" ? enabled : !enabled;
    unlock();
    return enabled;
}
void BoxMarkers::stop(bool handover)
{
    lock();
    shared_->stop = handover ? ingame::startup_handover : 1;
    unlock();
    for (unsigned i = 0; i < 100; ++i)
    {
        lock();
        const auto busy = shared_->running;
        unlock();
        if (!busy)
            return;
        Sleep(10);
    }
    throw std::runtime_error("In-game restoration is still pending. Restart NST before attaching another client");
}
