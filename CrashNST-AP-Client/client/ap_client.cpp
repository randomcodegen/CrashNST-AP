#include "client_runtime.h"
#include <wincrypt.h>
#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
namespace
{
    constexpr char expected_sha256[] = "170c61d37fb5277dd94b3e41f7a918f227b76bc67aa338f5c97f63ac317b6af3";
    std::optional<std::string> file_sha256(const std::filesystem::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return std::nullopt;
        HCRYPTPROV provider = 0;
        HCRYPTHASH hash = 0;
        if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
            return std::nullopt;
        bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != 0;
        std::array<char, 65536> buffer{};
        while (ok && file)
        {
            file.read(buffer.data(), buffer.size());
            const auto count = file.gcount();
            if (count > 0)
                ok = CryptHashData(hash, reinterpret_cast<const BYTE *>(buffer.data()), static_cast<DWORD>(count), 0) !=
                     0;
        }
        ok = ok && !file.bad();
        std::array<BYTE, 32> digest{};
        DWORD length = static_cast<DWORD>(digest.size());
        if (ok)
            ok = CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &length, 0) != 0 && length == digest.size();
        if (hash)
            CryptDestroyHash(hash);
        CryptReleaseContext(provider, 0);
        if (!ok)
            return std::nullopt;
        std::ostringstream result;
        for (BYTE byte : digest)
            result << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(byte);
        return result.str();
    }

} // namespace
void ClientRuntime::log_status(ClientTick &tick)
{
    const auto item_count = tick.items.size(), check_count = tick.checked.size();
    const auto last_item = item_count ? tick.items.back() : std::pair<std::int64_t, int>{};
    const auto last_check = check_count ? *tick.checked.rbegin() : 0;
    if (first || tick.status != previous || item_count != previous_items || check_count != previous_checks)
    {
        std::cout << "Connection status " << static_cast<int>(tick.status) << ", game PID " << pid
                  << ", received items " << item_count << ", checked locations " << check_count;
        if (item_count)
            std::cout << ", last item " << last_item.first << " (sender slot " << last_item.second << ')';
        if (check_count)
            std::cout << ", highest checked ID " << last_check;
        std::cout << std::endl;
        previous = tick.status;

        previous_items = item_count;
        previous_checks = check_count;
        first = false;
    }
}

void ClientRuntime::wait_for_commands()
{
    for (unsigned wait = 0; wait < 100 && running; ++wait)
    {
        if (!console_commands.empty())
            break;
        if (box_markers)
        {
            const auto commands = box_markers->commands();
            for (const auto &command : commands)
                console_commands.push_back(command);
            if (!console_commands.empty())
                break;
        }
        Sleep(10);
        if (wait % 10 == 9)
        {
            // wake quicker during attachment and scene changes. keep steady-state updates at 1 frame
            if ((session.ready() && !session_failed && (!level_routing || !level_routing->ready())) ||
                (previous_zone.hub == 1 && !c1_map))
                break;
            if (zone_base)
                try
                {
                    if (!(read_active_zone(GetCurrentProcess(), zone_base) == previous_zone))
                        break;
                }
                catch (const std::exception &)
                {
                    // the regular update reports errors and handles an unloading scene.
                    break;
                }
        }
    }
}

void ClientRuntime::finish()
{
    crate_events.reset();
    if (box_markers)
        try
        {
            box_markers->stop();
        }
        catch (const std::exception &e)
        {
            std::cerr << e.what() << std::endl;
        }
    box_markers.reset();
    if (level_routing)
    {
        try
        {
            level_routing->detach();
        }
        catch (const std::exception &error)
        {
            std::cerr << "Entrance routing restoration: " << error.what() << ". Restart NST.\n";
        }
    }
    // Restore engine ownership queries before asking the game to rebuild its live caches.
    for (auto &[name, access] : gem_access)
    {
        try
        {
            access->restore_query();
        }
        catch (const std::exception &error)
        {
            std::cerr << "Gem ownership query restoration: " << error.what() << std::endl;
        }
    }
    if (gem_refresh)
    {
        try
        {
            gem_refresh->request();
            for (unsigned i = 0; i < 100 && !gem_refresh->refreshed(); ++i)
                Sleep(10);
            if (!gem_refresh->refreshed())
                std::cerr << "Game gem cache restoration pending. Re-enter the level.\n";
        }
        catch (const std::exception &error)
        {
            std::cerr << "Game gem cache restoration: " << error.what() << std::endl;
        }
        gem_refresh.reset();
    }
    gem_access.clear();
    if (cloud && !session_failed)
    {
        try
        {
            cloud->tick(AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated, true);
            for (unsigned i = 0;
                 i < 100 && cloud->uploading() && AP_GetConnectionStatus() == AP_ConnectionStatus::Authenticated; ++i)
            {
                Sleep(20);
                std::deque<std::string> packets;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    packets.swap(ap_packets);
                    ap_packet_bytes = 0;
                }
                for (const auto &raw : packets)
                {
                    Json::Value messages;
                    Json::Reader reader;
                    if (!reader.parse(raw, messages) || !messages.isArray())
                        continue;
                    for (const auto &message : messages)
                        cloud->receive(message);
                }
            }
            if (cloud->uploading())
                std::cerr << "Final AP progress acknowledgement pending.\n"
                             "Local working saves and upload journal retained for reconnect.\n";
        }
        catch (const std::exception &error)
        {
            std::cerr << "Final AP progress sync: " << error.what() << ". Local saves and progress retained.\n";
        }
    }
}

int ClientRuntime::run()
{
    if (const auto result = initialize())
        return result;
    const std::map<CheckKey, std::int64_t> empty_checks;
    const std::set<std::string> empty_levels;
    const std::map<std::string, std::string> empty_placements;
    while (running)
    {
        handle_commands();
        const auto status = AP_GetConnectionStatus();
        process_packets(status);
        update_bridge(status);
        ClientTick tick{status,
                        session.items(),
                        session.checked(),
                        seed ? seed->checks : empty_checks,
                        seed ? seed->levels : empty_levels,
                        seed ? seed->placements : empty_placements,
                        status == AP_ConnectionStatus::Authenticated};
        tick.cleared = session.pending();
        tick.cleared.insert(tick.checked.begin(), tick.checked.end());
        update_routing(tick);
        update_display(tick);
        update_saves(tick);
        update_zone(tick);
        update_c2(tick);
        const auto hub = update_c3_gates(tick);
        update_c1(tick);
        update_c3_access(tick, hub);
        update_ownership(tick);
        log_status(tick);
        wait_for_commands();
        drain_crates(tick);
    }
    finish();
    return 0;
}

bool nst_verified_executable(const std::filesystem::path &path)
{
    return path.is_absolute() && file_sha256(path) == expected_sha256;
}
int nst_client_main(std::string server, std::string slot)
{
    ClientRuntime runtime(std::move(server), std::move(slot));
    return runtime.run();
}
