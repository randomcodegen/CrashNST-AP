#include "game_hooks.h"
#include "verified_memory.h"
#include "game_memory.h"

#include <array>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>
#include <tlhelp32.h>

namespace
{
    template <typename T>
    T read(HANDLE process, std::uintptr_t address)
    {
        T result{};
        SIZE_T count = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<void *>(address), &result, sizeof(result), &count) ||
            count != sizeof(result))
            throw std::runtime_error("Cannot read NST gate memory");
        return result;
    }

    template <typename T>
    void write(HANDLE process, std::uintptr_t address, const T &expected,
               const T &replacement)
    {
        if (read<T>(process, address) != expected)
            throw std::runtime_error("NST gate changed before write");
        SIZE_T count = 0;
        if (!WriteProcessMemory(process, reinterpret_cast<void *>(address), &replacement,
                                sizeof(replacement), &count) ||
            count != sizeof(replacement) ||
            read<T>(process, address) != replacement)
            throw std::runtime_error("NST gate write was not verified");
    }

    void restore_boss_route(HANDLE process, std::uintptr_t address, std::uintptr_t remote,
                            std::uintptr_t original)
    {
        const auto current = read<std::uintptr_t>(process, address);
        if (current == original)
            return;
        if (current != remote)
            throw std::runtime_error("Boss event route changed to an unknown handler. Restoration refused");
        write(process, address, remote, original);
    }

    bool matches_lift_data(HANDLE process, std::uintptr_t address,
                           const std::array<unsigned char, 232> &expected, unsigned floor)
    {
        auto actual = read<std::array<unsigned char, 232>>(process, address);
        
        for (unsigned direction = 0; direction < 2; ++direction)
            if (c2_lift_direction_allowed(floor, direction))
            {
                const unsigned offset = direction ? 120 : 168;
                // the lift's two permission pointers can potentially be cleared by our override. other pointers must match.
                std::uintptr_t current = 0, original = 0;
                std::memcpy(&current, actual.data() + offset, 8);
                std::memcpy(&original, expected.data() + offset, 8);
                if (current && current != original)
                    return false;
                std::copy_n(expected.begin() + offset, 8, actual.begin() + offset);
            }
        // engine reference counts can change independently of the two permission fields
        return std::equal(expected.begin(), expected.begin() + 8, actual.begin()) &&
               std::equal(expected.begin() + 16, expected.end(), actual.begin() + 16);
    }
}

C1MapAccess::C1MapAccess(DWORD pid, C1MapDiscovery found)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for C1 map access");
    try
    {
        before_ = probe_c1_map_at(process_, found);
        if (!before_.entry.hub_active)
            throw std::runtime_error("C1 map is inactive");
        constexpr unsigned entry_size = 34 * 256 + 40;
        // one destination-test slot per map spot, then a 40-byte input-recovery node and two secret-path slots
        remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, nullptr, entry_size + 512,
                                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!remote_)
            throw std::runtime_error("Cannot allocate C1 map event copies");
        secret_ = remote_ + entry_size;
        auto copied = c1_map_entry_chain(before_, remote_);
        const auto secrets = c1_map_secret_chain(before_, secret_);
        copied.insert(copied.end(), secrets.begin(), secrets.end());
        verified_copy(process_, remote_, copied.data(), copied.size(), "C1 map event copies were not verified");
        const auto current = probe_c1_map_at(process_, found);
        if (!current.entry.hub_active || current.entry.meta != before_.entry.meta || current.secrets.meta != before_.secrets.meta)
            throw std::runtime_error("C1 map changed before installation");
        try
        {
            write(process_, before_.entry.entry_output, before_.entry.original_output, remote_);
        }
        catch (...)
        {
            try
            {
                restore_boss_route(process_, before_.entry.entry_output, remote_, before_.entry.original_output);
            }
            catch (...)
            {
                std::cerr << "C1 map installation rollback failed. Restart NST.\n";
            }
            throw;
        }
        installed_ = true;
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

CrateEvents::CrateEvents(DWORD pid, std::uintptr_t meta, std::uintptr_t identity)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for crate events");
    try
    {
        before_ = probe_crate_at(process_, meta);
        before_.identity = identity;
        // Layout: RX vtable/node/adapter page, RW queue header page, then 1024 records of 256 bytes
        remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, nullptr, 8192 + 1024 * 256, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!remote_)
            throw std::runtime_error("Cannot allocate crate event queue");
        queue_ = remote_ + 4096;
        auto code = crate_break_stub(before_, queue_);
        if (code.size() > 3000)
            throw std::runtime_error("Crate event code exceeds allocation");
        const auto execute = remote_ + 512;
        auto table = before_.vtable;
        // VSC execute is vtable slot 22 (+176). the copied node's +32 link retains the original continuation
        std::memcpy(table.data() + 176, &execute, 8);
        auto node = before_.node;
        std::memcpy(node.data(), &remote_, 8);
        std::memcpy(node.data() + 32, &before_.original, 8);
        const auto copy = [&](std::uintptr_t address, const void *bytes, std::size_t size)
        {
            verified_copy(process_, address, bytes, size, "Crate event copy was not verified");
        };
        copy(remote_, table.data(), table.size());
        copy(remote_ + 256, node.data(), node.size());
        copy(execute, code.data(), code.size());
        DWORD old = 0;
        if (!VirtualProtectEx(process_, reinterpret_cast<void *>(remote_), 4096, PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(process_, reinterpret_cast<void *>(execute), code.size()))
            throw std::runtime_error("Cannot protect crate event code");
        probe_crate_at(process_, meta);
        try
        {
            write(process_, before_.route, before_.original, remote_ + 256);
        }
        catch (...)
        {
            restore_boss_route(process_, before_.route, remote_ + 256, before_.original);
            throw;
        }
        installed_ = true;
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}
CrateEvents::~CrateEvents()
{
    if (!process_)
        return;
    try
    {
        detach();
    }
    catch (const std::exception &error)
    {
        std::cerr << "Crate event restoration: " << error.what() << std::endl;
    }
    CloseHandle(process_);
}
void CrateEvents::detach()
{
    if (!installed_)
        return;
    probe_crate_at(process_, before_.meta, remote_ + 256);
    restore_boss_route(process_, before_.route, remote_ + 256, before_.original);
    installed_ = false;
}
std::vector<CrateBreak> CrateEvents::drain()
{
    if (!installed_)
        return {};
    probe_crate_at(process_, before_.meta, remote_ + 256);
    const auto header = read<std::array<std::uint64_t, 3>>(process_, queue_);
    // Header: events reserved, events consumed, overflow flag
    // Each record's sequence number is written last to mark it ready to read
    if (header[2] || header[0] < consumed_ || header[0] - consumed_ > 1024)
        throw std::runtime_error("Crate event queue overflow. Restart client before breaking more AP boxes");
    std::vector<CrateBreak> result;
    for (; consumed_ < header[0]; ++consumed_)
    {
        // Record: sequ num at +0, position at +16, crate name at +32 and zone name at +128
        const auto address = queue_ + 4096 + (consumed_ & 1023) * 256;
        const auto record = read<std::array<unsigned char, 256>>(process_, address);
        std::uint64_t ticket = 0;
        std::memcpy(&ticket, record.data(), 8);
        if (ticket != consumed_ + 1)
            break; // record is still being filled
        CrateBreak event;
        std::copy_n(record.begin() + 16, 12, event.position.begin());
        for (auto [offset, target] : {std::pair<unsigned, std::string *>{32, &event.name}, {128, &event.zone}})
        {
            const auto first = record.begin() + offset, end = std::find(first, first + 96, 0);
            if (end == first + 96)
                throw std::runtime_error("Unterminated crate event identity");
            target->assign(first, end);
        }
        if (read<std::uint64_t>(process_, address) != ticket)
            throw std::runtime_error("Crate event changed during read");
        result.push_back(std::move(event));
    }
    const auto old = read<std::uint64_t>(process_, queue_ + 8);
    if (old != consumed_)
        write(process_, queue_ + 8, old, consumed_);
    return result;
}

C1MapAccess::~C1MapAccess()
{
    if (!process_)
        return;
    try
    {
        detach();
    }
    catch (const std::exception &error)
    {
        std::cerr << "C1 map restoration: " << error.what() << ". Restart NST.\n";
    }
    CloseHandle(process_);
}

bool C1MapAccess::update(const std::set<std::string> &allowed, bool traversal)
{
    if (!installed_)
        return false;
    const auto current = probe_c1_map_at(process_, before_.discovery, remote_, secret_, true);
    if (!current.entry.hub_active)
        return false;
    if (current.entry.entry_output != before_.entry.entry_output || current.secrets.meta != before_.secrets.meta ||
        current.entry.destinations.size() != before_.entry.destinations.size())
        throw std::runtime_error("C1 map graph reloaded. Rediscovery required");
    const auto recovery = remote_ + 34 * 256;
    for (unsigned i = 0; i < 34; ++i)
    {
        if (current.entry.destinations[i].zone != before_.entry.destinations[i].zone)
            throw std::runtime_error("C1 map owner changed before access update");
        const auto branch = remote_ + i * 256 + 32;
        const auto old = read<std::uintptr_t>(process_, branch);
        if (old != recovery && old != before_.entry.original_output)
            throw std::runtime_error("C1 map dispatcher has an unknown branch");
        const auto desired = allowed.count(before_.entry.destinations[i].key) ? before_.entry.original_output : recovery;
        if (old != desired)
            write(process_, branch, old, desired);
    }
    if (traversal != traversal_)
    {
        try
        {
            write(process_, before_.progression_route, traversal_ ? before_.bounds : before_.original_progression,
                  traversal ? before_.bounds : before_.original_progression);
            write(process_, before_.secrets.entry_output, traversal_ ? secret_ : before_.secrets.original_output,
                  traversal ? secret_ : before_.secrets.original_output);
            write(process_, before_.shortcut_route, traversal_ ? before_.shortcut_move : before_.shortcut_denied,
                  traversal ? before_.shortcut_move : before_.shortcut_denied);
        }
        catch (...)
        {
            try
            {
                restore_boss_route(process_, before_.progression_route, before_.bounds, before_.original_progression);
                restore_boss_route(process_, before_.secrets.entry_output, secret_, before_.secrets.original_output);
                restore_boss_route(process_, before_.shortcut_route, before_.shortcut_move, before_.shortcut_denied);
                traversal_ = false;
            }
            catch (...)
            {
                std::cerr << "C1 traversal rollback failed. Restart NST.\n";
            }
            throw;
        }
        traversal_ = traversal;
    }
    return true;
}

void C1MapAccess::detach()
{
    if (!installed_)
        return;
    verify_c1_map_restore(process_, before_);
    restore_boss_route(process_, before_.progression_route, before_.bounds, before_.original_progression);
    restore_boss_route(process_, before_.secrets.entry_output, secret_, before_.secrets.original_output);
    restore_boss_route(process_, before_.shortcut_route, before_.shortcut_move, before_.shortcut_denied);
    traversal_ = false;
    restore_boss_route(process_, before_.entry.entry_output, remote_, before_.entry.original_output);
    installed_ = false;
}

namespace
{
    bool thread_exited(HANDLE thread)
    {
        DWORD code = STILL_ACTIVE;
        return GetExitCodeThread(thread, &code) && code != STILL_ACTIVE;
    }
    // pause NST threads only for the five-byte instruction replacement, never during a scan or test phase
    void patch_ability_call(HANDLE process, DWORD pid, std::uintptr_t site,
                            const std::array<unsigned char, 5> &expected,
                            const std::array<unsigned char, 5> &replacement)
    {
        // collect handles before pausing. in-process threads may own CRT/allocator
        // locks, so: use only stack buffers and kernel APIs until every thread resumes!!!
        std::array<HANDLE, 512> threads{};
        std::array<bool, 512> paused{};
        unsigned count = 0;
        const char *failure = nullptr;
        bool resume_failed = false;
        const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot enumerate NST threads for ability patch");
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        if (!Thread32First(snapshot, &entry))
            failure = "Cannot read NST thread snapshot";
        else
        {
            do
            {
                if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == GetCurrentThreadId())
                    continue;
                if (count == threads.size())
                {
                    failure = "NST exceeds the instruction-patch thread bound";
                    break;
                }
                const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
                if (!thread)
                {
                    // loading workers can exit between the snapshot and opening their handle
                    if (GetLastError() == ERROR_INVALID_PARAMETER)
                        continue;
                    failure = "Cannot open NST thread for ability patch";
                    break;
                }
                const auto owner = GetProcessIdOfThread(thread);
                if (owner != pid)
                {
                    const bool gone = thread_exited(thread);
                    CloseHandle(thread);
                    if (!owner && !gone)
                    {
                        failure = "Cannot verify NST patch thread owner";
                        break;
                    }
                    continue;
                }
                threads[count++] = thread;
            } while (Thread32Next(snapshot, &entry));
            if (!failure && GetLastError() != ERROR_NO_MORE_FILES)
                failure = "NST thread enumeration was incomplete";
        }
        CloseHandle(snapshot);
        for (unsigned i = 0; !failure && i < count; ++i)
        {
            if (SuspendThread(threads[i]) == DWORD(-1))
            {
                if (thread_exited(threads[i]))
                    continue;
                failure = "Cannot pause NST thread";
                break;
            }
            paused[i] = true;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(threads[i], &context))
            {
                if (!thread_exited(threads[i]))
                    failure = "Cannot inspect paused NST thread";
            }
            else if (context.Rip >= site - 1 && context.Rip < site + 5)
                failure = "NST thread is inside the ability call. Retry attachment";
        }
        std::array<unsigned char, 5> actual{};
        SIZE_T bytes = 0;
        DWORD protection = 0;
        const auto verify = [&](const auto &value)
        { return ReadProcessMemory(process, reinterpret_cast<void *>(site), actual.data(), 5, &bytes) && bytes == 5 && actual == value; };
        if (!failure && !verify(expected))
            failure = "Ability call instruction changed before patch";
        if (!failure && !VirtualProtectEx(process, reinterpret_cast<void *>(site), 5, PAGE_EXECUTE_READWRITE, &protection))
            failure = "Cannot protect ability call instruction";
        if (!failure)
        {
            if (!WriteProcessMemory(process, reinterpret_cast<void *>(site), replacement.data(), 5, &bytes) || bytes != 5 || !verify(replacement))
            {
                failure = "Ability instruction write failed";
                if (!WriteProcessMemory(process, reinterpret_cast<void *>(site), expected.data(), 5, &bytes) || bytes != 5 || !verify(expected))
                    failure = "Ability instruction rollback failed. Restart NST immediately";
            }
            DWORD ignored = 0;
            const bool protected_again = VirtualProtectEx(process, reinterpret_cast<void *>(site), 5, protection, &ignored);
            const bool flushed = FlushInstructionCache(process, reinterpret_cast<void *>(site), 5);
            if (!protected_again || !flushed)
                failure = "Ability instruction protection/cache restoration failed. Restart NST";
        }
        for (unsigned i = 0; i < count; ++i)
        {
            if (paused[i] && ResumeThread(threads[i]) == DWORD(-1) && !thread_exited(threads[i]))
                resume_failed = true;
            CloseHandle(threads[i]);
        }
        if (resume_failed)
            throw std::runtime_error("NST thread resume failed. Restart NST immediately");
        if (failure)
            throw std::runtime_error(failure);
    }
    constexpr std::array<unsigned char, 5> original_jump_call{{0xe8, 0xf1, 0x60, 0x03, 0}};
    constexpr std::array<unsigned char, 5> original_ability_refresh_call{{0xe8, 0x59, 0x02, 0x32, 0}};
    constexpr std::array<unsigned, 5> ability_query_items{{0, 1, 3, 4, 5}};

    std::vector<unsigned char> routing_stub(std::uintptr_t state, std::uintptr_t manager, std::uintptr_t current_zone,
                                            std::uintptr_t loader, std::uintptr_t zone_vtable, std::uintptr_t setter = 0,
                                            std::uintptr_t int_getter = 0)
    {
        std::vector<unsigned char> c;
        const auto emit = [&](std::initializer_list<unsigned char> bytes)
        { c.insert(c.end(), bytes); };
        const auto imm = [&](std::uintptr_t v)
        {const auto p=reinterpret_cast<const unsigned char*>(&v);c.insert(c.end(),p,p+8); };
        // branch emits 0F cc rel32 (84=equal, 85=not equal. bind measures from the end of that displacement.
        const auto branch = [&](unsigned char op)
        {emit({0x0f,op,0,0,0,0});return c.size()-4; };
        const auto bind = [&](std::size_t at, std::size_t to)
        {const auto d=static_cast<std::int32_t>(to-at-4);std::memcpy(c.data()+at,&d,4); };
        const auto jump = [&]
        {emit({0xe9,0,0,0,0});return c.size()-4; };
        // immutable records at state+256: source, destination, source hub. live return state at +8/+16
        // preserve RBX/RSI/RDI/R12 and reserve aligned call shadow space before invoking engine helpers
        emit({0x53, 0x56, 0x57, 0x41, 0x54, 0x48, 0x83, 0xec, 40, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xf2, 0x49, 0xbc});
        imm(state);
        emit({0x48, 0x85, 0xdb});
        std::vector<std::size_t> done{branch(0x84)};
        emit({0x41, 0x80, 0x7c, 0x24, 4, 1});
        done.push_back(branch(0x85));
        emit({0x48, 0xb8});
        imm(zone_vtable);
        emit({0x48, 0x39, 0x03});
        done.push_back(branch(0x85));
        emit({0x48, 0xb8});
        imm(manager);
        emit({0x48, 0x8b, 0x08, 0x48, 0x85, 0xc9});
        done.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(current_zone);
        emit({0xff, 0xd0, 0x48, 0x8b, 0xf8, 0x48, 0x85, 0xff});
        done.push_back(branch(0x84));
        emit({0x48, 0x3b, 0xdf});
        const auto restart = branch(0x84); // restart/checkpoint retains its destination
        // native secret selectors distinguish the three fixed alternate sections from shuffled hub portals
        if (int_getter)
        {
            emit({0x49, 0x3b, 0x7c, 0x24, 40});
            const auto c2_hub = branch(0x84);
            emit({0x49, 0x3b, 0x7c, 0x24, 48});
            std::vector<std::size_t> ordinary{branch(0x85)};
            bind(c2_hub, c.size());
            emit({0x49, 0x8b, 0x4c, 0x24, 112, 0x48, 0x85, 0xc9});
            ordinary.push_back(branch(0x84));
            emit({0x48, 0xb8}); imm(int_getter);
            emit({0xff, 0xd0, 0xff, 0xc8, 0x83, 0xf8, 8});
            ordinary.push_back(branch(0x87));
            emit({0x49, 0x3b, 0xbc, 0xc4, 0x80, 0x0c, 0, 0}); // matching hub at state+3200+selector*8
            ordinary.push_back(branch(0x85));
            emit({0x49, 0x3b, 0x9c, 0xc4, 0x30, 0x0c, 0, 0}); // target at state+3120+selector*8
            ordinary.push_back(branch(0x85));
            emit({0x49, 0x83, 0x7c, 0x24, 8, 0});
            const auto origin = branch(0x85);
            emit({0x49, 0x89, 0x7c, 0x24, 8});
            bind(origin, c.size());
            // a return to this secret room needs its actual stage for the native portal-exit animation
            emit({0x49, 0x3b, 0x7c, 0x24, 8});
            const auto other_campaign = branch(0x85);
            emit({0x49, 0x89, 0x9c, 0x24, 144, 0, 0, 0, 0x49, 0x89, 0xbc, 0x24, 152, 0, 0, 0});
            bind(other_campaign, c.size());
            emit({0x49, 0x89, 0x5c, 0x24, 16});
            emit({0xff, 0xc0, 0x41, 0x89, 0x84, 0x24, 208, 0, 0, 0}); // retain the alternate entrance across Game Over
            done.push_back(jump());
            for (auto at : ordinary) bind(at, c.size());
        }
        // secret transfers keep the game's secret-exit discovery. denied destinations return to the entrance's hub.
        emit({0x49, 0x8d, 0x8c, 0x24, 0, 11, 0, 0, 0xba, 4, 0, 0, 0});
        const auto secret_loop = c.size();
        emit({0x48, 0x3b, 0x59, 8});
        const auto secret_match = branch(0x84);
        emit({0x48, 0x83, 0xc1, 32, 0xff, 0xca});
        const auto next_secret = branch(0x85);
        bind(next_secret, secret_loop);
        const auto ordinary_load = jump();
        bind(secret_match, c.size());
        emit({0x48, 0x8b, 0x41, 16, 0x49, 0x3b, 0x7c, 0x24, 16});
        const auto original_origin = branch(0x85);
        emit({0x49, 0x83, 0x7c, 0x24, 8, 0});
        const auto missing_origin = branch(0x84);
        emit({0x49, 0x8b, 0x44, 0x24, 8});
        bind(original_origin, c.size());
        bind(missing_origin, c.size());
        emit({0x49, 0x89, 0x44, 0x24, 8, 0x83, 0x79, 24, 1});
        const auto secret_locked = branch(0x85);
        emit({0x48, 0x3b, 0x39});
        const auto original_path = branch(0x84);
        emit({0x83, 0x79, 28, 1});
        const auto wrong_path = branch(0x85);
        emit({0x48, 0x3b, 0x79, 16});
        const auto wrong_hub = branch(0x85);
        bind(original_path, c.size());
        emit({0x49, 0x89, 0x5c, 0x24, 16});
        const auto secret_allowed = jump();
        const auto refused = c.size();
        bind(secret_locked, refused);
        bind(wrong_path, refused);
        bind(wrong_hub, refused);
        emit({0x49, 0x8b, 0x5c, 0x24, 8, 0x49, 0xc7, 0x44, 0x24, 8, 0, 0, 0, 0, 0x49, 0xc7, 0x44, 0x24, 16, 0, 0, 0, 0});
        if (setter)
        {
            emit({0x49, 0x8b, 0x4c, 0x24, 112, 0x48, 0x85, 0xc9});
            const auto no_variable = branch(0x84);
            emit({0x31, 0xd2, 0x48, 0xb8});
            imm(setter);
            emit({0xff, 0xd0});
            bind(no_variable, c.size());
        }
        const auto secret_refused = jump();
        bind(ordinary_load, c.size());
        // C2 secret exits load its warp room before the destination. dont mistake for a stage exit.
        std::vector<std::size_t> ordinary_return;
        if (int_getter)
        {
            emit({0x49, 0x3b, 0x5c, 0x24, 40});
            ordinary_return.push_back(branch(0x85));
            emit({0x49, 0x8b, 0x4c, 0x24, 112, 0x48, 0x85, 0xc9});
            ordinary_return.push_back(branch(0x84));
            emit({0x48, 0xb8});
            imm(int_getter);
            emit({0xff, 0xd0, 0xff, 0xc8, 0x83, 0xf8, 4});
            ordinary_return.push_back(branch(0x87));
            // each selector is valid only for its own original source level
            emit({0x48, 0xc1, 0xe0, 3, 0x49, 0x8b, 0x84, 0x04, 0, 12, 0, 0, 0x48, 0x3b, 0xf8});
            ordinary_return.push_back(branch(0x85));
            emit({0x49, 0x89, 0x5c, 0x24, 16});
            done.push_back(jump());
            for (auto at : ordinary_return)
                bind(at, c.size());
        }
        // return only from the level we actually entered. boss/menu loads keep their own route.
        emit({0x49, 0x83, 0x7c, 0x24, 8, 0});
        const auto no_return = branch(0x84);
        emit({0x49, 0x3b, 0x7c, 0x24, 16});
        const auto not_return = branch(0x85);
        std::vector<std::size_t> hubs;
        for (unsigned offset : {32u, 40u, 48u})
        {
            emit({0x49, 0x3b, 0x5c, 0x24, static_cast<unsigned char>(offset)});
            hubs.push_back(branch(0x84));
        }
        const auto not_hub = jump();
        const auto return_to_hub = c.size();
        emit({0x49, 0x8b, 0x5c, 0x24, 8, 0x49, 0xc7, 0x44, 0x24, 8, 0, 0, 0, 0});
        const auto returned = jump();
        const auto lookup = c.size();
        bind(no_return, lookup);
        bind(not_return, lookup);
        bind(not_hub, lookup);
        for (auto at : hubs)
            bind(at, return_to_hub);
        emit({0x41, 0x8b, 0x04, 0x24, 0x3d, 101, 0, 0, 0});
        done.push_back(branch(0x87));
        emit({0x49, 0x8d, 0x8c, 0x24, 0, 1, 0, 0});
        const auto loop = c.size();
        emit({0x85, 0xc0});
        done.push_back(branch(0x84));
        emit({0x48, 0x3b, 0x19});
        const auto matched = branch(0x84);
        emit({0x48, 0x83, 0xc1, 24, 0xff, 0xc8});
        const auto again = jump();
        bind(again, loop);
        bind(matched, c.size());
        emit({0x48, 0x3b, 0x79, 16});
        done.push_back(branch(0x85)); // only physical hub entrances are shuffled.
        emit({0x41, 0xc7, 0x84, 0x24, 208, 0, 0, 0, 0, 0, 0, 0});
        if (setter)
        {
            // route immediately after startup. defer only the optional secret-selector reset.
            emit({0x49, 0x83, 0x7c, 0x24, 112, 0});
            const auto unbound = branch(0x84);
            emit({0x48, 0x3b, 0x59, 8});
            const auto identity = branch(0x84);
            emit({0x48, 0x89, 0x4c, 0x24, 32, 0x49, 0x8b, 0x4c, 0x24, 112, 0x31, 0xd2, 0x48, 0xb8});
            imm(setter);
            emit({0xff, 0xd0, 0x48, 0x8b, 0x4c, 0x24, 32});
            bind(identity, c.size());
            bind(unbound, c.size());
        }
        emit({0x48, 0x8b, 0x41, 16, 0x49, 0x89, 0x44, 0x24, 8, 0x49, 0x89, 0x84, 0x24, 152, 0, 0, 0,
              0x48, 0x8b, 0x01, 0x49, 0x89, 0x84, 0x24, 144, 0, 0, 0,
              0x49, 0x89, 0x84, 0x24, 176, 0, 0, 0, // latest ordinary entrance, sampled for local persistence
              0x48, 0x8b, 0x59, 8, 0x49, 0x89, 0x5c, 0x24, 16});
        const auto finish = c.size();
        bind(restart, finish);
        bind(returned, finish);
        if (setter && int_getter)
        {
            // full reload loses the dynamic checkpoint. restore secret-start selector, not coordinates.
            emit({0x49, 0x3b, 0x7c, 0x24, 16});
            std::vector<std::size_t> skip{branch(0x85)};
            emit({0x41, 0x8b, 0x84, 0x24, 208, 0, 0, 0, 0xff, 0xc8, 0x83, 0xf8, 8});
            skip.push_back(branch(0x87));
            emit({0x49, 0x3b, 0xbc, 0xc4, 0x30, 0x0c, 0, 0});
            skip.push_back(branch(0x85));
            emit({0x48, 0x3b, 0xdf});
            const auto reload = branch(0x84);
            emit({0x49, 0x3b, 0x9c, 0xc4, 0x80, 0x0c, 0, 0});
            skip.push_back(branch(0x85));
            bind(reload, c.size());
            emit({0xff, 0xc0, 0x89, 0x44, 0x24, 32, 0x49, 0x8b, 0x4c, 0x24, 112, 0x48, 0x85, 0xc9});
            skip.push_back(branch(0x84));
            emit({0x48, 0xb8}); imm(int_getter);
            emit({0xff, 0xd0, 0x8b, 0x54, 0x24, 32, 0x3b, 0xc2});
            skip.push_back(branch(0x84));
            emit({0x49, 0x8b, 0x4c, 0x24, 112, 0x48, 0xb8}); imm(setter);
            emit({0xff, 0xd0});
            for (auto at : skip) bind(at, c.size());
        }
        for (auto at : done)
            bind(at, c.size());
        bind(secret_allowed, c.size());
        bind(secret_refused, c.size());
        emit({0x48, 0x8b, 0xcb, 0x48, 0x8b, 0xd6, 0x48, 0x83, 0xc4, 40, 0x41, 0x5c, 0x5f, 0x5e, 0x5b, 0x48, 0xb8});
        imm(loader);
        emit({0xff, 0xe0});
        return c;
    }

    // hub scripts select map spots/return portals from the previous level. supply the physical entrance.
    std::vector<unsigned char> previous_entrance_stub(std::uintptr_t state, std::uintptr_t manager,
                                                      std::uintptr_t current_zone, std::uintptr_t original, std::uintptr_t assign, bool index)
    {
        std::vector<unsigned char> c;
        const auto emit = [&](std::initializer_list<unsigned char> bytes)
        { c.insert(c.end(), bytes); };
        const auto imm = [&](std::uintptr_t v)
        {const auto p=reinterpret_cast<const unsigned char*>(&v);c.insert(c.end(),p,p+8); };
        const auto branch = [&](unsigned char op)
        {emit({0x0f,op,0,0,0,0});return c.size()-4; };
        emit({0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 32, 0x48, 0x8b, 0xd9, 0x48, 0x8b, 0xfa, 0x48, 0xb8});
        imm(original);
        emit({0xff, 0xd0, 0x48, 0xbe});
        imm(state);
        emit({0x80, 0x7e, 4, 1});
        std::vector<std::size_t> done{branch(0x85)};
        emit({0x48, 0xb8});
        imm(manager);
        emit({0x48, 0x8b, 0x08, 0x48, 0x85, 0xc9});
        done.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(current_zone);
        emit({0xff, 0xd0});
        std::vector<std::size_t> hubs;
        for (unsigned offset : {32u, 40u, 48u})
        {
            emit({0x48, 0x3b, 0x46, static_cast<unsigned char>(offset)});
            hubs.push_back(branch(0x84));
        }
        emit({0xe9, 0, 0, 0, 0});
        done.push_back(c.size() - 4);
        for (auto at : hubs)
        {
            const auto d = static_cast<std::int32_t>(c.size() - at - 4);
            std::memcpy(c.data() + at, &d, 4);
        }
        // no matching entrance history means a fresh hub spawn
        emit({0x31, 0xd2, 0x48, 0x3b, 0x86, 152, 0, 0, 0});
        const auto other_hub = branch(0x85);
        emit({0x48, 0x8b, 0x96, 144, 0, 0, 0});
        const auto delta = static_cast<std::int32_t>(c.size() - other_hub - 4);
        std::memcpy(c.data() + other_hub, &delta, 4);
        if (index)
        {
            emit({0xb8, 0xff, 0xff, 0xff, 0xff, 0x48, 0x85, 0xd2});
            const auto no_entrance = branch(0x84);
            emit({0x8b, 0x82, 88, 1, 0, 0}); // CZoneInfo::_progressionIndex, +344.
            const auto d = static_cast<std::int32_t>(c.size() - no_entrance - 4);
            std::memcpy(c.data() + no_entrance, &d, 4);
            emit({0x89, 0x03, 0x48, 0x8b, 0xdf});
        }
        emit({0x48, 0x8b, 0x0b, 0x48, 0x89, 0x13, 0x48, 0xb8});
        imm(assign);
        emit({0xff, 0xd0});
        for (auto at : done)
        {
            const auto d = static_cast<std::int32_t>(c.size() - at - 4);
            std::memcpy(c.data() + at, &d, 4);
        }
        emit({0x48, 0x83, 0xc4, 32, 0x5f, 0x5e, 0x5b, 0xc3});
        return c;
    }

    // pause-menu Quit resolves its hub before issuing loadmap directly
    std::vector<unsigned char> quit_campaign_stub(std::uintptr_t state, std::uintptr_t manager,
                                                  std::uintptr_t current_zone, std::uintptr_t campaign)
    {
        std::vector<unsigned char> c;
        const auto emit = [&](std::initializer_list<unsigned char> bytes)
        { c.insert(c.end(), bytes); };
        const auto imm = [&](std::uintptr_t v)
        {const auto p=reinterpret_cast<const unsigned char*>(&v);c.insert(c.end(),p,p+8); };
        const auto branch = [&](unsigned char op)
        {emit({0x0f,op,0,0,0,0});return c.size()-4; };
        const auto bind = [&](std::size_t at, std::size_t to)
        {const auto d=static_cast<std::int32_t>(to-at-4);std::memcpy(c.data()+at,&d,4); };
        emit({0x53, 0x56, 0x48, 0x83, 0xec, 40, 0x48, 0xb8});
        imm(campaign);
        emit({0xff, 0xd0, 0x8b, 0xd8, 0x48, 0xbe});
        imm(state);
        emit({0x80, 0x7e, 4, 1});
        std::vector<std::size_t> done{branch(0x85)};
        emit({0x48, 0x83, 0x7e, 8, 0});
        done.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(manager);
        emit({0x48, 0x8b, 0x08, 0x48, 0x85, 0xc9});
        done.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(current_zone);
        emit({0xff, 0xd0, 0x48, 0x3b, 0x46, 16});
        done.push_back(branch(0x85));
        emit({0x48, 0x8b, 0x46, 8});
        std::vector<std::size_t> hubs;
        for (unsigned offset : {32u, 40u, 48u})
        {
            emit({0x48, 0x3b, 0x46, static_cast<unsigned char>(offset)});
            hubs.push_back(branch(0x84));
        }
        emit({0xe9, 0, 0, 0, 0});
        done.push_back(c.size() - 4);
        for (unsigned i = 0; i < 3; ++i)
        {
            bind(hubs[i], c.size());
            emit({0xbb, static_cast<unsigned char>(i + 1), 0, 0, 0, 0xe9, 0, 0, 0, 0});
            done.push_back(c.size() - 4);
        }
        const auto finish = c.size();
        for (auto at : done)
            bind(at, finish);
        // keep return state intact: the GUI asks once before confirmation/fading, then again to load.
        emit({0x8b, 0xc3, 0x48, 0x83, 0xc4, 40, 0x5e, 0x5b, 0xc3});
        return c;
    }

    // the secret teleporter fades through 74e7c0 -> 146a80, which otherwise falls back to saved progression.
    std::vector<unsigned char> secret_fade_stub(std::uintptr_t state, std::uintptr_t manager, std::uintptr_t current_zone,
                                                std::uintptr_t int_getter, std::uintptr_t original, std::uintptr_t string_create, std::uintptr_t string_free)
    {
        std::vector<unsigned char> c;
        const auto emit = [&](std::initializer_list<unsigned char> bytes)
        { c.insert(c.end(), bytes); };
        const auto imm = [&](std::uintptr_t v)
        {const auto p=reinterpret_cast<const unsigned char*>(&v);c.insert(c.end(),p,p+8); };
        const auto branch = [&](unsigned char op)
        {emit({0x0f,op,0,0,0,0});return c.size()-4; };
        const auto bind = [&](std::size_t at, std::size_t to)
        {const auto d=static_cast<std::int32_t>(to-at-4);std::memcpy(c.data()+at,&d,4); };
        emit({0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 80, 0x48, 0x8b, 0xd9, 0x48, 0xbe});
        imm(state);
        emit({0x80, 0x7e, 4, 1});
        std::vector<std::size_t> fallback{branch(0x85)};
        emit({0x48, 0x83, 0x7e, 8, 0});
        fallback.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(manager);
        emit({0x48, 0x8b, 0x08, 0x48, 0x85, 0xc9});
        fallback.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(current_zone);
        emit({0xff, 0xd0, 0x48, 0x8b, 0xf8, 0x48, 0x3b, 0x46, 16});
        fallback.push_back(branch(0x85));
        emit({0x48, 0x8b, 0x4e, 112, 0x48, 0x85, 0xc9});
        fallback.push_back(branch(0x84));
        emit({0x48, 0xb8});
        imm(int_getter);
        emit({0xff, 0xd0, 0xff, 0xc8, 0x83, 0xf8, 4});
        fallback.push_back(branch(0x87));
        emit({0x48, 0xc1, 0xe0, 3, 0x48, 0x3b, 0xbc, 0x06, 0, 12, 0, 0});
        fallback.push_back(branch(0x85));
        emit({0x48, 0x8b, 0x46, 40, 0x48, 0x85, 0xc0});
        fallback.push_back(branch(0x84));
        emit({0x48, 0x8b, 0x50, 16, 0x48, 0x85, 0xd2});
        fallback.push_back(branch(0x84));
        // 146a80 reads only the string at +24. a private stack argument avoids changing its shared fade state
        emit({0x48, 0xc7, 0x44, 0x24, 56, 0, 0, 0, 0, 0x48, 0x8d, 0x4c, 0x24, 56, 0x48, 0xb8});
        imm(string_create);
        emit({0xff, 0xd0});
        emit({0x48, 0x8b, 0x46, 40, 0x48, 0x89, 0x46, 16, 0xff, 0x86, 160, 0, 0, 0,
              0x48, 0x8d, 0x4c, 0x24, 32, 0x48, 0xb8});
        imm(original);
        emit({0xff, 0xd0});
        emit({0x48, 0x8b, 0x4c, 0x24, 56, 0x48, 0x85, 0xc9});
        const auto empty = branch(0x84);
        emit({0x48, 0x83, 0xe9, 16, 0x48, 0xb8});
        imm(string_free);
        emit({0xff, 0xd0});
        bind(empty, c.size());
        emit({0x48, 0x83, 0xc4, 80, 0x5f, 0x5e, 0x5b, 0xc3});
        for (auto at : fallback)
            bind(at, c.size());
        emit({0x48, 0x8b, 0xcb, 0x48, 0x83, 0xc4, 80, 0x5f, 0x5e, 0x5b, 0x48, 0xb8});
        imm(original);
        emit({0xff, 0xe0});
        return c;
    }

    // campaign menu loads restore the remembered physical entrance in the selected campaign's hub
    std::vector<unsigned char> campaign_start_stub(std::uintptr_t state, std::uintptr_t getter,
                                                   std::uintptr_t zone_vtable, std::uintptr_t int_getter = 0)
    {
        std::vector<unsigned char> c;
        const auto emit = [&](std::initializer_list<unsigned char> bytes)
        { c.insert(c.end(), bytes); };
        const auto imm = [&](std::uintptr_t v)
        {const auto p=reinterpret_cast<const unsigned char*>(&v);c.insert(c.end(),p,p+8); };
        const auto branch = [&](unsigned char op)
        {emit({0x0f,op,0,0,0,0});return c.size()-4; };
        const auto bind = [&](std::size_t at, std::size_t to)
        {const auto d=static_cast<std::int32_t>(to-at-4);std::memcpy(c.data()+at,&d,4); };
        const auto path = [&](const char *expected, std::vector<std::size_t> &fallback)
        {
            emit({0x48, 0x8b, 0x48, 16, 0x48, 0x85, 0xc9});
            fallback.push_back(branch(0x84));
            for (unsigned i = 0; i < std::strlen(expected); ++i)
            {
                emit({0x80, 0x79, static_cast<unsigned char>(i), static_cast<unsigned char>(expected[i])});
                fallback.push_back(branch(0x85)); // a shorter path fails at its terminator before any further byte is read
            }
        };
        emit({0x53, 0x48, 0x83, 0xec, 48, 0x48, 0xb8});
        imm(getter);
        emit({0xff, 0xd0, 0x48, 0xbb});
        imm(state);
        emit({0x48, 0x85, 0xc0});
        std::vector<std::size_t> done{branch(0x84)};
        emit({0x80, 0x7b, 4, 1});
        done.push_back(branch(0x85));
        emit({0x48, 0xba});
        imm(zone_vtable);
        emit({0x48, 0x39, 0x10});
        done.push_back(branch(0x85));
        std::vector<std::size_t> selected;
        for (unsigned game = 1; game <= 3; ++game)
        {
            std::vector<std::size_t> next;
            const auto prefix = "crash" + std::to_string(game) + "/";
            path(prefix.c_str(), next);
            emit({0x48, 0x8b, 0x4b, static_cast<unsigned char>(24 + game * 8),
                  0x4c, 0x8b, 0x83, static_cast<unsigned char>(176 + game * 8), 0, 0, 0, 0xe9, 0, 0, 0, 0});
            selected.push_back(c.size() - 4);
            for (auto at : next)
                bind(at, c.size());
        }
        emit({0xe9, 0, 0, 0, 0});
        done.push_back(c.size() - 4);
        for (auto at : selected)
            bind(at, c.size());
        emit({0x48, 0x85, 0xc9});
        done.push_back(branch(0x84));
        emit({0x48, 0x39, 0x11});
        done.push_back(branch(0x85));
        emit({0x48, 0x8b, 0xc1, 0x48, 0xc7, 0x43, 8, 0, 0, 0, 0, 0x48, 0xc7, 0x43, 16, 0, 0, 0, 0,
              0x48, 0xc7, 0x83, 144, 0, 0, 0, 0, 0, 0, 0, 0x48, 0xc7, 0x83, 152, 0, 0, 0, 0, 0, 0, 0});
        emit({0x4d, 0x85, 0xc0});
        std::vector<std::size_t> no_resume{branch(0x84)};
        emit({0x49, 0x39, 0x10});
        no_resume.push_back(branch(0x85));
        if (int_getter)
        {
            // C2 secret-room selectors keep their saved spawn
            emit({0x48, 0x3b, 0x43, 40});
            const auto other_game = branch(0x85);
            emit({0x48, 0x8b, 0x4b, 112, 0x48, 0x85, 0xc9});
            const auto no_selector = branch(0x84);
            emit({0x48, 0x89, 0x44, 0x24, 32, 0x4c, 0x89, 0x44, 0x24, 40, 0x48, 0xb8});
            imm(int_getter);
            emit({0xff, 0xd0, 0xff, 0xc8, 0x83, 0xf8, 4,
                  0x48, 0x8b, 0x44, 0x24, 32, 0x4c, 0x8b, 0x44, 0x24, 40});
            no_resume.push_back(branch(0x86));
            bind(other_game, c.size());
            bind(no_selector, c.size());
        }
        emit({0x4c, 0x89, 0x83, 144, 0, 0, 0, 0x48, 0x89, 0x83, 152, 0, 0, 0});
        for (auto at : no_resume)
            bind(at, c.size());
        emit({0xff, 0x83, 136, 0, 0, 0}); // Acknowledgement counts actual redirects, not attachment.
        for (auto at : done)
        {
            const auto delta = static_cast<std::int32_t>(c.size() - at - 4);
            std::memcpy(c.data() + at, &delta, 4);
        }
        emit({0x48, 0x83, 0xc4, 48, 0x5b, 0xc3});
        return c;
    }

}

void patch_relative_call(HANDLE process, DWORD pid, std::uintptr_t site,
                       const std::array<unsigned char, 5> &expected, const std::array<unsigned char, 5> &replacement)
{
    patch_ability_call(process, pid, site, expected, replacement);
}
LevelRouting::LevelRouting(DWORD pid, const std::map<std::string, std::string> &placements) : pid_(pid)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for entrance routing");
    try
    {
        base_ = nst_module_base(process_);
        zones_ = read_zone_registry(process_);
        const auto zone_for = [&](const std::string &key)
        {
            const ZoneRecord *found = nullptr;
            for (const auto &zone : zones_)
                if (zone.key == key && zone.path.find("intro") == std::string::npos)
                {
                    if (found && found->address != zone.address)
                    {
                        std::ostringstream message;
                        message << "Ambiguous level destination: " << key << " [" << found->path << " @ 0x"
                                << std::hex << found->address << "] and [" << zone.path << " @ 0x" << zone.address << ']';
                        throw std::runtime_error(message.str());
                    }
                    found = &zone;
                }
            if (!found)
                throw std::runtime_error("Level destination missing: " + key);
            return found->address;
        };
        // verified executable offsets. this order also determines calls_ and restoration in detach()
        const std::array<std::uintptr_t, 8> sites{{
            base_ + 0x75ff64, // conditional map load. the preceding 0x0f byte stays unchanged
            base_ + 0x75ffa8, // direct map-loader call
            base_ + 0x7627e7, //tail jump to the map loader
            base_ + 0x830fd1, // pause-menu quit: choose the physical entrance's campaign hub
            base_ + 0x81a6d0, // campaign start: redirect intro/first-stage destinations to hubs
            base_ + 0x274c85, // hub spawn: query the physical previous entrance
            base_ + 0x26fc5c, // hub spawn: query its physical progression index
            base_ + 0x74e85e // C2 secret-exit fade: return through the secret warp room
        }};
        // original opcode/displacement bytes, in sites order, used to verify and restore each patch.
        const std::array<std::array<unsigned char, 5>, 8> original{{{{0x85, 0x77, 0, 0, 0}}, {{0xe8, 0x33, 0, 0, 0}}, {{0xe9, 0xf4, 0xd7, 0xff, 0xff}}, {{0xe8, 0x2a, 0x04, 0x90, 0xff}}, {{0xe8, 0x5b, 0x5d, 0xd0, 0xff}}, {{0xe8, 0x96, 0x26, 0x4f, 0}}, {{0xe8, 0x8f, 0x6f, 0x4e, 0}}, {{0xe8, 0x1d, 0x82, 0x9f, 0xff}}}};
        // also verify surrounding functions before trusting these patch locations.
        if (read<std::array<unsigned char, 10>>(process_, base_ + 0x753d80) !=
                std::array<unsigned char, 10>{{0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20}} ||
            read<unsigned char>(process_, base_ + 0x75ff63) != 0x0f ||
            read<std::array<unsigned char, 11>>(process_, base_ + 0x75ffe0) !=
                std::array<unsigned char, 11>{{0x48, 0x8b, 0xc4, 0x53, 0x48, 0x81, 0xec, 0xc0, 0, 0, 0}} ||
            read<std::array<unsigned char, 17>>(process_, base_ + 0x830fc0) !=
                std::array<unsigned char, 17>{{0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0x62, 0x71, 0x7f, 0xff, 0x48, 0x8b, 0xc8}} ||
            read<std::array<unsigned char, 16>>(process_, base_ + 0x81a6c0) !=
                std::array<unsigned char, 16>{{0x40, 0x53, 0x48, 0x81, 0xec, 0xc0, 0, 0, 0, 0x48, 0x81, 0xc1, 0xa0, 0, 0, 0}} ||
            read<std::array<unsigned char, 12>>(process_, base_ + 0x146a80) !=
                std::array<unsigned char, 12>{{0x40, 0x53, 0x48, 0x81, 0xec, 0xc0, 0, 0, 0, 0x48, 0x8b, 0xd9}} ||
            read<std::array<unsigned char, 4>>(process_, base_ + 0x146a99) != std::array<unsigned char, 4>{{0x48, 0x8d, 0x53, 24}})
            throw std::runtime_error("Game map loader differs from the verified build");
        for (unsigned i = 0; i < sites.size(); ++i)
            if (read<std::array<unsigned char, 5>>(process_, sites[i]) != original[i])
                throw std::runtime_error("Map-load script route has another override");
        // search both directions on 64 KiB allocation boundaries, relative calls/jumps must stay within signed 32-bit reach
        const auto aligned = sites[0] & ~std::uintptr_t(0xffff);
        for (std::uintptr_t distance = 0x10000; !remote_ && distance < 0x7fff0000; distance += 0x10000)
            for (const auto address : {aligned + distance, aligned > distance ? aligned - distance : 0})
            {
                MEMORY_BASIC_INFORMATION region{};
                if (!address || !VirtualQueryEx(process_, reinterpret_cast<void *>(address), &region, sizeof(region)) || region.State != MEM_FREE || region.RegionSize < 8192)
                    continue;
                remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, reinterpret_cast<void *>(address), 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (remote_)
                    break;
            }
        if (!remote_)
            throw std::runtime_error("Cannot allocate entrance-routing dispatcher");
        if (read<std::array<unsigned char, 6>>(process_, base_ + 0x4e4710) != std::array<unsigned char, 6>{{0x40, 0x53, 0x48, 0x83, 0xec, 0x40}})
            throw std::runtime_error("Game secret-selector getter differs from the verified build");
        auto code = routing_stub(state(), base_ + 0x1a5b690, base_ + 0x753d80, base_ + 0x75ffe0, base_ + 0x13d0fb0, base_ + 0x502ed0, base_ + 0x4e4710);
        if (code.size() > 1024)
            throw std::runtime_error("Entrance routing code exceeds its reserved region");
        // INT3 padding fixes the entry offsets of subsequent adapters and traps unintended execution between them
        code.resize(1024, 0xcc);
        // Dispatcher slots: routing +0, C1 previous zone/index +1024/+1536, Quit +2048, fade +2560, start +3072
        for (unsigned i = 0; i < 2; ++i)
        {
            const auto previous = previous_entrance_stub(state(), base_ + 0x1a5b690, base_ + 0x753d80,
                                                         base_ + (i ? 0x756bf0 : 0x767320), base_ + 0xdf1100, i != 0);
            code.insert(code.end(), previous.begin(), previous.end());
            if (code.size() > 1536 + i * 512)
                throw std::runtime_error("Previous entrance query exceeds its reserved region");
            code.resize(1536 + i * 512, 0xcc);
        }
        code.resize(2048, 0xcc);
        const auto quit = quit_campaign_stub(state(), base_ + 0x1a5b690, base_ + 0x753d80, base_ + 0x131400);
        code.insert(code.end(), quit.begin(), quit.end());
        if (code.size() > 2560)
            throw std::runtime_error("Quit routing overlaps secret-fade code");
        code.resize(2560, 0xcc);
        const auto fade = secret_fade_stub(state(), base_ + 0x1a5b690, base_ + 0x753d80, base_ + 0x4e4710,
                                           base_ + 0x146a80, base_ + 0xdce380, base_ + 0xe6bbf0);
        code.insert(code.end(), fade.begin(), fade.end());
        if (code.size() > 3072)
            throw std::runtime_error("Secret-fade routing overlaps campaign-start code");
        code.resize(3072, 0xcc);
        const auto start = campaign_start_stub(state(), base_ + 0x520430, base_ + 0x13d0fb0, base_ + 0x4e4710);
        code.insert(code.end(), start.begin(), start.end());
        if (code.size() > 4096)
            throw std::runtime_error("Entrance routing code exceeds its executable region");
        std::array<unsigned char, 4096> data{};
        // separate RW page: count/enabled at +0/+4, return hub/destination +8/+16, campaign hubs +32/+40/+48
        // +176 publishes the last ordinary entrance. +184/+192/+200 hold Continue entrances.
        // +208 retains an active alternate selector.
        const auto put = [&](unsigned off, std::uintptr_t v)
        { std::memcpy(data.data() + off, &v, 8); };
        unsigned count = 0;
        // each 24-byte placement at +256 stores source zone, destination zone and the physical entrance's hub.
        for (const auto &[source, destination] : placements)
        {
            if (count >= 101)
                throw std::runtime_error("Too many level placements");
            if (source[1] == '1' && !c1_entrance_progression_valid(source, read<int>(process_, zone_for(source) + 344)))
                throw std::runtime_error("C1 physical entrance has an invalid game progression index: " + source);
            put(256 + count * 24, zone_for(source));
            entrances_.emplace(source, zone_for(source));
            put(264 + count * 24, zone_for(destination));
            put(272 + count * 24, zone_for("L" + source.substr(1, 1) + "00"));
            ++count;
        }
        std::memcpy(data.data(), &count, 4);
        data[4] = 1;
        put(112, variable_);
        if (std::any_of(placements.begin(), placements.end(), [](const auto &p)
                        { return p.first[1] == '1'; }))
        {
            put(120, zone_for("L101"));
        }
        if (std::any_of(placements.begin(), placements.end(), [](const auto &p)
                        { return p.first[1] == '2'; }))
            put(128, zone_for("L200"));
        for (unsigned i = 0; i < 3; ++i)
            put(32 + i * 8, zone_for("L" + std::to_string(i + 1) + "00"));
        for (unsigned i = 0; i < secret_stages.size(); ++i)
        {
            // secret slots at +2816 have three zone pointers, an allowed flag at +24 and a C2-path flag at +28
            const auto &[destination, source] = secret_stages[i];
            const auto off = 2816 + i * 32;
            put(off, zone_for(source));
            put(off + 8, zone_for(destination));
            put(off + 16, zone_for("L" + std::string(destination).substr(1, 1) + "00"));
            data[off + 28] = destination[1] == '2';
        }
        // ECrashSecretZones 1..5 identify the five C2 secret-warp-room exits
        for (unsigned i = 0; i < c2_secret_entrances.size(); ++i)
        {
            put(3072 + i * 8, zone_for(c2_secret_entrances[i].second));
            if (i < 3)
            {
                put(3120 + i * 8, zone_for(c2_secret_entrances[i].first));
                put(3200 + i * 8, zone_for("L200"));
            }
        }
        // C3 buttons 27/29 share normal zone objects.
        for (const auto &[key, selector] : std::array<std::pair<const char *, unsigned>, 2>{{{"L307", 7}, {"L319", 9}}})
        {
            put(3120 + (selector - 1) * 8, zone_for(key));
            put(3200 + (selector - 1) * 8, zone_for("L300"));
        }
        SIZE_T copied = 0;
        if (!WriteProcessMemory(process_, reinterpret_cast<void *>(remote_), code.data(), code.size(), &copied) || copied != code.size() ||
            !WriteProcessMemory(process_, reinterpret_cast<void *>(state()), data.data(), data.size(), &copied) || copied != data.size() ||
            read<std::array<unsigned char, 4096>>(process_, state()) != data)
            throw std::runtime_error("Entrance-routing data failed readback");
        std::vector<unsigned char> check(code.size());
        if (!ReadProcessMemory(process_, reinterpret_cast<void *>(remote_), check.data(), check.size(), &copied) || copied != check.size() || check != code)
            throw std::runtime_error("Entrance-routing code failed readback");
        DWORD old = 0;
        if (!VirtualProtectEx(process_, reinterpret_cast<void *>(remote_), 4096, PAGE_EXECUTE_READ, &old) || !FlushInstructionCache(process_, reinterpret_cast<void *>(remote_), code.size()))
            throw std::runtime_error("Cannot protect entrance routing code");
        for (unsigned i = 0; i < sites.size(); ++i)
        {
            calls_[i] = ability_jump_call(sites[i], remote_ + (i == 7 ? 2560 : i == 6 ? 1536
                                                                           : i == 5   ? 1024
                                                                           : i == 4   ? 3072
                                                                           : i == 3   ? 2048
                                                                                      : 0));
            // reuse the rel32 encoder, retaining call/JMP/JNE semantics (JNE's leading 0F is outside sites[0])
            calls_[i][0] = original[i][0];
            try
            {
                patch_ability_call(process_, pid_, sites[i], original[i], calls_[i]);
                ++installed_;
            }
            catch (...)
            {
                if (read<std::array<unsigned char, 5>>(process_, sites[i]) == calls_[i])
                    ++installed_;
                throw;
            }
        }
    }
    catch (...)
    {
        try
        {
            detach();
        }
        catch (...)
        {
            std::cerr << "Entrance routing rollback failed. Restart NST.\n";
        }
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}
LevelRouting::~LevelRouting()
{
    if (process_)
    {
        try
        {
            detach();
        }
        catch (const std::exception &e)
        {
            std::cerr << "Entrance routing restoration: " << e.what() << std::endl;
        }
        CloseHandle(process_);
    }
}
void LevelRouting::detach()
{
    if (!installed_)
        return;
    const auto enabled = read<unsigned char>(process_, state() + 4);
    if (enabled == 1)
        write(process_, state() + 4, enabled, static_cast<unsigned char>(0));
    else if (enabled != 0)
        throw std::runtime_error("Entrance dispatcher ownership changed during restoration");
    // same site order and original bytes as the constructor.
    const std::array<unsigned, 8> rvas{{0x75ff64, 0x75ffa8, 0x7627e7, 0x830fd1, 0x81a6d0, 0x274c85, 0x26fc5c, 0x74e85e}};
    const std::array<std::array<unsigned char, 5>, 8> original{{{{0x85, 0x77, 0, 0, 0}}, {{0xe8, 0x33, 0, 0, 0}}, {{0xe9, 0xf4, 0xd7, 0xff, 0xff}}, {{0xe8, 0x2a, 0x04, 0x90, 0xff}}, {{0xe8, 0x5b, 0x5d, 0xd0, 0xff}}, {{0xe8, 0x96, 0x26, 0x4f, 0}}, {{0xe8, 0x8f, 0x6f, 0x4e, 0}}, {{0xe8, 0x1d, 0x82, 0x9f, 0xff}}}};
    while (installed_) // unwind only patches successfully installed.
    {
        const auto i = installed_ - 1;
        patch_ability_call(process_, pid_, base_ + rvas[i], calls_[i], original[i]);
        --installed_;
    }
}

bool LevelRouting::bind_secret(const std::array<unsigned char, 48> &handle)
{
    const auto variable = probe_secret_variable(process_, handle);
    if (variable == variable_)
        return false;
    write(process_, state() + 112, variable_, variable);
    variable_ = variable;
    return true;
}
unsigned LevelRouting::start_count() const { return read<unsigned>(process_, state() + 136); }
unsigned LevelRouting::secret_return_count() const { return read<unsigned>(process_, state() + 160); }

std::string LevelRouting::last_entrance() const
{
    const auto last = read<std::uintptr_t>(process_, state() + 176);
    if (!last)
        return {};
    for (const auto &[key, zone] : entrances_)
        if (zone == last)
            return key;
    throw std::runtime_error("Last entrance is outside the routing table");
}
void LevelRouting::resume_entrances(const std::map<char, std::string> &entrances)
{
    std::array<std::uintptr_t, 3> desired{};
    for (const auto &[game, key] : entrances)
    {
        const auto found = entrances_.find(key);
        if (game < '1' || game > '3' || key.size() != 4 || key[1] != game || found == entrances_.end())
            throw std::runtime_error("Continue entrance is outside the routing table");
        desired[game - '1'] = found->second;
    }
    for (unsigned i = 0; i < desired.size(); ++i)
    {
        const auto address = state() + 184 + i * 8;
        const auto current = read<std::uintptr_t>(process_, address);
        if (current != desired[i])
            write(process_, address, current, desired[i]);
    }
}

void LevelRouting::secrets(const std::set<std::string> &allowed)
{
    if (!installed_ || read<unsigned char>(process_, state() + 4) != 1)
        throw std::runtime_error("Entrance-routing dispatcher is inactive");
    for (unsigned i = 0; i < secret_stages.size(); ++i)
    {
        const auto address = state() + 2816 + i * 32 + 24;
        const auto current = read<unsigned>(process_, address);
        if (current > 1)
            throw std::runtime_error("Secret entrance ownership changed");
        const unsigned desired = allowed.count(secret_stages[i].first) ? 1 : 0;
        if (current != desired)
            write(process_, address, current, desired);
    }
}

AbilityAccess::AbilityAccess(DWORD pid, const GemDiscovery &found, const AbilityOwnership &owned, std::uintptr_t jump_filter)
    : pid_(pid), found_(found), owned_(owned), jump_filter_(jump_filter)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for ability access");
    try
    {
        before_ = probe_abilities_at(process_, found_);
        try
        {
            filler_ = probe_filler_at(process_, found_.hud_meta);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Filler delivery waiting: " << e.what() << std::endl;
        }
        menu_ = probe_ability_menu_at(process_, found_);
        if (menu_.meta)
        {
            menu_templates_ = probe_campaign_templates_at(process_, found_);
            for (unsigned i = 0; i < 6; ++i)
                menu_templates_.handles[i] = read<std::array<unsigned char, 48>>(process_, found_.upgrades[i]);
            menu_remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!menu_remote_)
                throw std::runtime_error("Cannot allocate ability-menu display dispatcher");
            const auto bytes = ability_menu_chain(menu_templates_, menu_, menu_remote_, owned);
            verified_copy(process_, menu_remote_, bytes.data(), bytes.size(), "Ability-menu copies failed readback");
        }
        // the x64 relative call needs a target within two GiB. reserve two pages: RX code, RW ownership byte
        const auto aligned = before_.jump_call & ~std::uintptr_t(0xffff);
        for (std::uintptr_t distance = 0x10000; !remote_ && distance < 0x7fff0000; distance += 0x10000)
        {
            for (const auto address : {aligned + distance, aligned > distance ? aligned - distance : 0})
            {
                if (!address || address >= 0x7fff00000000ULL - 8192)
                    continue;
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQueryEx(process_, reinterpret_cast<void *>(address), &region, sizeof(region)) ||
                    region.State != MEM_FREE || region.RegionSize < 8192)
                    continue;
                remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, reinterpret_cast<void *>(address), 8192,
                                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (remote_)
                    break;
            }
        }
        if (!remote_)
            throw std::runtime_error("Cannot allocate a nearby Double Jump adapter");
        call_ = ability_jump_call(before_.jump_call, remote_);
        // RX page: Double Jump +0, power refresh +64, filler delivery +1024. following page holds mutable state
        refresh_call_ = ability_jump_call(before_.refresh_call, remote_ + 64);
        auto code = ability_jump_stub(before_, remote_, jump_filter_);
        code.resize(64, 0xcc);
        const auto refresh_code = ability_refresh_stub(before_, remote_, filler_.meta ? remote_ + 1024 : 0);
        code.insert(code.end(), refresh_code.begin(), refresh_code.end());
        if (filler_.meta)
        {
            if (code.size() > 1024)
                throw std::runtime_error("Ability refresh overlaps filler code");
            code.resize(1024, 0xcc);
            const auto dispatch = filler_stub(before_, filler_, remote_);
            code.insert(code.end(), dispatch.begin(), dispatch.end());
        }
        if (code.size() > 4096)
            throw std::runtime_error("Ability/filler dispatch exceeds code page");
        verified_copy(process_, remote_, code.data(), code.size(), "Double Jump adapter failed code readback");
        DWORD ignored = 0;
        if (!VirtualProtectEx(process_, reinterpret_cast<void *>(remote_), 4096, PAGE_EXECUTE_READ, &ignored) ||
            !FlushInstructionCache(process_, reinterpret_cast<void *>(remote_), code.size()))
            throw std::runtime_error("Cannot finalize Double Jump adapter code");
        write(process_, remote_ + 4096, static_cast<unsigned char>(0), static_cast<unsigned char>(owned[2]));
        const auto method = ability_refresh_method(before_, remote_ + 4352);
        verified_copy(process_, remote_ + 4352, method.data(), method.size(), "Ability-cache refresh method failed readback");
        if (filler_.meta)
            for (unsigned kind = 0; kind < 2; ++kind)
            {
                const auto address = remote_ + 5120 + kind * 384;
                const auto bytes = filler_method(before_, filler_, kind, address);
                verified_copy(process_, address, bytes.data(), bytes.size(), "Filler-reward script method failed readback");
            }
        // RW state: owned +0, request/completed generations +4/+8, component +16, busy/enabled +24/+25.
        write(process_, remote_ + 4100, 0u, generation_);
        write(process_, remote_ + 4121, static_cast<unsigned char>(0), static_cast<unsigned char>(1));
        probe_abilities_at(process_, found_);
        installed_ = true; // rollback also covers partially installed query inputs
        try
        {
            if (menu_.meta)
            {
                probe_ability_menu_at(process_, found_);
                write(process_, menu_.route, menu_.original, menu_remote_);
            }
            for (unsigned i = 0; i < 5; ++i)
                write(process_, before_.inputs[i], before_.original[i], owned[ability_query_items[i]] ? before_.yes : before_.no);
            patch_ability_call(process_, pid_, before_.jump_call, original_jump_call, call_);
            patch_ability_call(process_, pid_, before_.refresh_call, original_ability_refresh_call, refresh_call_);
            if (filler_.meta)
                std::cout << "1-Up/Wumpa reward delivery attached. Receipts wait for an active level.\n";
            std::cout << (menu_.meta ? "AP ability-menu display attached. Close and reopen the pause menu after ownership changes.\n" : "Ability-menu graph not loaded. Gameplay refresh is active, menu display is pending discovery.\n")
                      << std::flush;
        }
        catch (...)
        {
            try
            {
                detach();
            }
            catch (...)
            {
                std::cerr << "Ability installation rollback failed. Restart NST.\n";
            }
            throw;
        }
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

AbilityAccess::~AbilityAccess()
{
    if (!process_)
        return;
    try
    {
        detach();
    }
    catch (const std::exception &error)
    {
        std::cerr << "Ability restoration: " << error.what() << ". Restart NST.\n";
    }
    CloseHandle(process_);
}

bool AbilityAccess::update(const AbilityOwnership &owned)
{
    if (!installed_)
        throw std::runtime_error("Ability adapter is detached");
    if (read<std::array<unsigned char, 5>>(process_, before_.jump_call) != call_ ||
        read<std::array<unsigned char, 5>>(process_, before_.refresh_call) != refresh_call_)
        throw std::runtime_error("Ability call has another override");
    try
    {
        probe_abilities_at(process_, found_, true, call_, refresh_call_);
        if (filler_.meta)
            probe_filler_at(process_, filler_.meta);
    }
    catch (const std::exception &)
    {
        // loading invalidates dynamic metadata before the process exits.
        // stop dispatch first, then let the caller detach the module hooks and discover the new compiled graph
        write(process_, remote_ + 4121, static_cast<unsigned char>(1), static_cast<unsigned char>(0));
        return false;
    }
    if (menu_.meta)
    {
        try
        {
            probe_ability_menu_at(process_, found_, menu_remote_);
        }
        catch (const std::exception &)
        {
            write(process_, remote_ + 4121, static_cast<unsigned char>(1), static_cast<unsigned char>(0));
            return false;
        }
        const auto expected = ability_menu_chain(menu_templates_, menu_, menu_remote_, owned_);
        for (unsigned campaign = 0; campaign < 3; ++campaign)
            for (unsigned item = 0; item < 6; ++item)
            {
                const auto off = campaign * 1024 + 256 + item * 128 + 32;
                std::uintptr_t branch = 0;
                std::memcpy(&branch, expected.data() + off, 8);
                if (read<std::uintptr_t>(process_, menu_remote_ + off) != branch)
                    throw std::runtime_error("Ability-menu dispatcher has another override");
            }
    }
    const auto expected_code = ability_jump_stub(before_, remote_, jump_filter_);
    const auto code = read<std::array<unsigned char, 35>>(process_, remote_);
    if (!std::equal(code.begin(), code.end(), expected_code.begin()))
        throw std::runtime_error("Double Jump adapter code changed externally");
    const auto expected_refresh = ability_refresh_stub(before_, remote_, filler_.meta ? remote_ + 1024 : 0);
    std::vector<unsigned char> actual_refresh(expected_refresh.size());
    SIZE_T count = 0;
    if (!ReadProcessMemory(process_, reinterpret_cast<void *>(remote_ + 64), actual_refresh.data(), actual_refresh.size(), &count) ||
        count != actual_refresh.size() || actual_refresh != expected_refresh ||
        read<unsigned>(process_, remote_ + 4100) != generation_ || read<unsigned char>(process_, remote_ + 4121) != 1)
        throw std::runtime_error("Ability-cache refresh adapter changed externally");
    if (filler_.meta)
    {
        const auto expected = filler_stub(before_, filler_, remote_);
        std::vector<unsigned char> actual(expected.size());
        if (!ReadProcessMemory(process_, reinterpret_cast<void *>(remote_ + 1024), actual.data(), actual.size(), &count) || count != actual.size() || actual != expected)
            throw std::runtime_error("Filler-reward machine code changed externally");
        for (unsigned kind = 0; kind < 2; ++kind)
        {
            const auto address = remote_ + 5120 + kind * 384;
            const auto bytes = filler_method(before_, filler_, kind, address, kind ? filler_amount_ : 1);
            std::vector<unsigned char> actual_method(bytes.size());
            if (!ReadProcessMemory(process_, reinterpret_cast<void *>(address), actual_method.data(), actual_method.size(), &count) || count != actual_method.size())
                throw std::runtime_error("Cannot read filler-reward script method");
            for (unsigned i = 0; i < bytes.size(); ++i)
            {
                // copied method/list/node headers contain live reference counts. their links remain checked.
                if ((i >= 8 && i < 16) || (i >= 40 && i < 48) || (i >= 88 && i < 96) ||
                    (i >= 152 && i < 160) || (i >= 200 && i < 208) || (i >= 232 && i < 240))
                    continue;
                if (actual_method[i] != bytes[i])
                    throw std::runtime_error("Filler-reward script method changed externally");
            }
        }
    }
    const auto expected_method = ability_refresh_method(before_, remote_ + 4352);
    std::vector<unsigned char> actual_method(expected_method.size());
    if (!ReadProcessMemory(process_, reinterpret_cast<void *>(remote_ + 4352), actual_method.data(), actual_method.size(), &count) || count != actual_method.size())
        throw std::runtime_error("Cannot read ability-refresh script method");
    for (unsigned i = 0; i < actual_method.size(); ++i)
    {
        // engine reference-count headers can change. executable links and list bounds cant.
        if ((i >= 8 && i < 16) || (i >= 40 && i < 48) || (i >= 88 && i < 96))
            continue;
        if (actual_method[i] != expected_method[i])
            throw std::runtime_error("Ability-cache refresh method changed externally");
    }
    for (unsigned i = 0; i < 5; ++i)
        if (read<std::uintptr_t>(process_, before_.inputs[i]) != (owned_[ability_query_items[i]] ? before_.yes : before_.no))
            throw std::runtime_error("Ability input has another override");
    if (read<unsigned char>(process_, remote_ + 4096) != owned_[2])
        throw std::runtime_error("Double Jump ownership changed externally");
    try
    {
        if (menu_.meta && owned_ != owned)
        {
            const auto previous = ability_menu_chain(menu_templates_, menu_, menu_remote_, owned_);
            const auto next = ability_menu_chain(menu_templates_, menu_, menu_remote_, owned);
            for (unsigned campaign = 0; campaign < 3; ++campaign)
                for (unsigned item = 0; item < 6; ++item)
                {
                    const auto off = campaign * 1024 + 256 + item * 128 + 32;
                    std::uintptr_t old = 0, desired = 0;
                    std::memcpy(&old, previous.data() + off, 8);
                    std::memcpy(&desired, next.data() + off, 8);
                    if (old != desired)
                        write(process_, menu_remote_ + off, old, desired);
                }
        }
        for (unsigned i = 0; i < 5; ++i)
            if (owned_[ability_query_items[i]] != owned[ability_query_items[i]])
                write(process_, before_.inputs[i], owned_[ability_query_items[i]] ? before_.yes : before_.no,
                      owned[ability_query_items[i]] ? before_.yes : before_.no);
        write(process_, remote_ + 4096, static_cast<unsigned char>(owned_[2]), static_cast<unsigned char>(owned[2]));
        if (owned_ != owned)
        {
            write(process_, remote_ + 4100, generation_, generation_ + 1);
            ++generation_;
        }
        owned_ = owned;
    }
    catch (...)
    {
        detach();
        throw;
    }
    return true;
}

bool AbilityAccess::refreshed() const
{
    return installed_ && read<unsigned>(process_, remote_ + 4104) == generation_;
}
bool AbilityAccess::filler_ready() const
{
    return installed_ && filler_.meta && read<unsigned char>(process_, remote_ + 4180) == 1;
}
bool AbilityAccess::filler_pending() const
{
    return filler_.meta && read<std::uint64_t>(process_, remote_ + 4160) != read<std::uint64_t>(process_, remote_ + 4168);
}
void AbilityAccess::queue_filler(unsigned index, std::int64_t item, std::uintptr_t zone)
{
    if (!installed_ || !filler_.meta || !zone || index >= 100000 || (item != 0xCB0000 + 2201 && item != 0xCB0000 + 2202) || filler_pending())
        throw std::runtime_error("Filler-reward delivery not ready");
    write(process_, remote_ + 4184, read<std::uintptr_t>(process_, remote_ + 4184), zone);
    const auto amount = wumpa_amount(index);
    if (item == 0xCB0000 + 2202)
    {
        write(process_, remote_ + 5504 + 216, static_cast<int>(filler_amount_), static_cast<int>(amount));
        filler_amount_ = amount;
    }
    const auto old = read<unsigned>(process_, remote_ + 4176);
    write(process_, remote_ + 4176, old, static_cast<unsigned>(item - (0xCB0000 + 2201)));
    const auto ticket = read<std::uint64_t>(process_, remote_ + 4160);
    write(process_, remote_ + 4160, ticket, static_cast<std::uint64_t>(index) + 1);
}

void AbilityAccess::detach()
{
    if (!installed_)
        return;
    const auto current = read<std::array<unsigned char, 5>>(process_, before_.jump_call);
    if (current != call_ && current != original_jump_call)
        throw std::runtime_error("Double Jump call has an unknown override");
    // restore module code even if the level's script allocation has already unloaded.
    const auto refresh = read<std::array<unsigned char, 5>>(process_, before_.refresh_call);
    if (refresh != refresh_call_ && refresh != original_ability_refresh_call)
        throw std::runtime_error("Ability refresh call has an unknown override");
    if (menu_.meta && menu_remote_)
    {
        try
        {
            probe_ability_menu_at(process_, found_, menu_remote_);
            restore_boss_route(process_, menu_.route, menu_remote_, menu_.original);
        }
        catch (const std::exception &error)
        {
            std::cout << "Ability-menu graph unavailable or changed. Skipping its route restoration: " << error.what() << ".\n";
        }
    }
    bool graph_verified = false;
    // if the graph still exists, let the game restore its unmodified ability cache before removing the hook
    try
    {
        probe_abilities_at(process_, found_, true, current, refresh);
        graph_verified = true;
        for (unsigned i = 0; i < 5; ++i)
        {
            const auto input = read<std::uintptr_t>(process_, before_.inputs[i]);
            if (input != before_.original[i])
                write(process_, before_.inputs[i], input, before_.original[i]);
        }
        if (refresh == refresh_call_ && read<unsigned char>(process_, remote_ + 4121) == 1)
        {
            write(process_, remote_ + 4100, generation_, generation_ + 1);
            ++generation_;
            for (unsigned i = 0; i < 100 && !refreshed(); ++i)
                Sleep(10);
            if (!refreshed())
                std::cerr << "Ability-cache restoration is pending. Re-enter the level if powers remain cached.\n";
        }
    }
    catch (const std::exception &error)
    {
        if (graph_verified)
            std::cerr << "Ability-cache refresh during detach: " << error.what() << ".\n";
        else
            std::cout << "Old ability graph unavailable. Skipping its data restoration.\n"
                      << std::flush;
    }
    // a damaged data page should not prevent restoration of the module calls
    try
    {
        const auto enabled = read<unsigned char>(process_, remote_ + 4121);
        if (enabled == 1)
            write(process_, remote_ + 4121, enabled, static_cast<unsigned char>(0));
        else if (enabled != 0)
            throw std::runtime_error("Unknown refresh enable value");
    }
    catch (const std::exception &error)
    {
        std::cerr << "Ability refresh disable: " << error.what() << ".\n";
    }
    if (refresh == refresh_call_)
        patch_ability_call(process_, pid_, before_.refresh_call, refresh_call_, original_ability_refresh_call);
    if (current == call_)
        patch_ability_call(process_, pid_, before_.jump_call, call_, original_jump_call);
    // code restoration is independent of asset lifetime, dont read unloaded graphs
    installed_ = false;
    if (graph_verified)
    {
        try
        {
            probe_abilities_at(process_, found_, true, original_jump_call);
            for (unsigned i = 0; i < 5; ++i)
            {
                const auto input = read<std::uintptr_t>(process_, before_.inputs[i]);
                if (input != before_.original[i])
                    write(process_, before_.inputs[i], input, before_.original[i]);
            }
        }
        catch (const std::exception &error)
        {
            std::cout << "Ability module hooks restored. Graph changed during restoration: " << error.what() << ".\n";
        }
    }
}

GemRouteAccess::GemRouteAccess(DWORD pid, const GemDiscovery &found, const std::string &name,
                               const GemOwnership &owned) : found_(found), owned_(owned)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for gem route access");
    try
    {
        templates_ = probe_gem_templates_at(process_, found);
        before_ = probe_gem_route_at(process_, name, found.graphs.at(name), 0, templates_.data_accessor_vtable);
        remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, nullptr, 4096,
                                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!remote_)
            throw std::runtime_error("Cannot allocate gem-query script copies");
        const auto bytes = gem_route_chain(templates_, before_, remote_, owned);
        verified_copy(process_, remote_, bytes.data(), bytes.size(), "Gem-query script copies failed readback");
        probe_gem_templates_at(process_, found_);
        probe_gem_route_at(process_, name, before_.meta, 0, templates_.data_accessor_vtable);
        try
        {
            if (before_.effect_route)
                write(process_, before_.effect_route, before_.effect_original, remote_ + 3072);
            write(process_, before_.route, before_.original, remote_);
        }
        catch (...)
        {
            try
            {
                restore_boss_route(process_, before_.route, remote_, before_.original);
            }
            catch (...)
            {
                std::cerr << "Gem route installation rollback failed. Restart NST.\n";
            }
            if (before_.effect_route)
            {
                try
                {
                    restore_boss_route(process_, before_.effect_route, remote_ + 3072, before_.effect_original);
                }
                catch (...)
                {
                    std::cerr << "Gem effect guard rollback failed. Restart NST.\n";
                }
            }
            throw;
        }
        installed_ = true;
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

GemRouteAccess::~GemRouteAccess()
{
    if (!process_)
        return;
    try
    {
        detach();
    }
    catch (const std::exception &error)
    {
        std::cerr << "Gem route restoration: " << error.what() << ". Restart NST.\n";
    }
    CloseHandle(process_);
}

void GemRouteAccess::update(const GemOwnership &owned)
{
    if (!installed_)
        throw std::runtime_error("Gem route adapter is detached");
    probe_gem_templates_at(process_, found_);
    probe_gem_route_at(process_, before_.name, before_.meta, remote_, templates_.data_accessor_vtable,
                       before_.effect_route ? remote_ + 3072 : 0);
    // the appended TestObject guard checks SpawnedVFX before allowing the original effect-spawn route
    if (before_.effect_route &&
        (read<std::uintptr_t>(process_, remote_ + 3072) != read<std::uintptr_t>(process_, before_.original) + 0x170 ||
         read<std::uintptr_t>(process_, remote_ + 3088) != before_.effect_accessor ||
         read<std::uintptr_t>(process_, remote_ + 3096) != 0 ||
         read<std::uintptr_t>(process_, remote_ + 3104) != before_.effect_original))
        throw std::runtime_error("Yellow teleporter duplicate-effect guard changed externally");
    // check every branch before changing any. dont overwrite unknown handlers
    // three 1024-byte campaign blocks contain six 128-byte color tests. +32 is each test's true output.
    for (unsigned game = 0; game < 3; ++game)
        for (unsigned color = 0; color < 6; ++color)
        {
            const auto branch = remote_ + game * 1024 + 256 + color * 128 + 32;
            if (read<std::uintptr_t>(process_, branch) != (owned_[game][color] ? before_.yes : before_.no))
                throw std::runtime_error("Gem-query dispatcher has an unknown branch");
        }
    try
    {
        for (unsigned game = 0; game < 3; ++game)
            for (unsigned color = 0; color < 6; ++color)
            {
                if (owned_[game][color] == owned[game][color])
                    continue;
                const auto branch = remote_ + game * 1024 + 256 + color * 128 + 32;
                write(process_, branch, owned_[game][color] ? before_.yes : before_.no,
                      owned[game][color] ? before_.yes : before_.no);
            }
        owned_ = owned;
    }
    catch (...)
    {
        detach(); // stop using the partial update. the next attachment starts from received items
        throw;
    }
}

void GemRouteAccess::detach()
{
    if (!installed_)
        return;
    try
    {
        probe_gem_route_at(process_, before_.name, before_.meta, remote_, templates_.data_accessor_vtable,
                           before_.effect_route ? remote_ + 3072 : 0);
    }
    catch (const std::exception &error)
    {
        std::cout << "Gem graph unavailable or changed. Skipping its route restoration: " << error.what() << ".\n";
        installed_ = false;
        return;
    }
    restore_boss_route(process_, before_.route, remote_, before_.original);
    if (before_.effect_route)
        restore_boss_route(process_, before_.effect_route, remote_ + 3072, before_.effect_original);
    installed_ = false;
}

void GemRouteAccess::restore_query()
{
    if (!installed_)
        return;
    try
    {
        probe_gem_route_at(process_, before_.name, before_.meta, remote_, templates_.data_accessor_vtable,
                           before_.effect_route ? remote_ + 3072 : 0);
    }
    catch (const std::exception &error)
    {
        std::cout << "Gem query unavailable or changed during game gem-cache refresh. Skipping: " << error.what() << ".\n";
        return;
    }
    restore_boss_route(process_, before_.route, remote_, before_.original);
}

namespace
{
    constexpr std::array<unsigned char, 5> original_gem_refresh_call{{0xe8, 0x12, 0x2d, 0x32, 0}};
}

GemRefreshAccess::GemRefreshAccess(DWORD pid, const GemDiscovery &found, std::vector<GemRouteProbe> jobs,
                                   const GemOwnership &owned, std::uintptr_t addon_tick) : pid_(pid), owned_(owned)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for gem refresh");
    try
    {
        before_ = probe_gem_refresh_at(process_, found);
        const auto aligned = before_.call & ~std::uintptr_t(0xffff);
        for (std::uintptr_t d = 0x10000; !remote_ && d < 0x7fff0000; d += 0x10000)
        {
            for (const auto a : {aligned + d, aligned > d ? aligned - d : 0})
            {
                if (!a || a >= 0x7fff00000000ULL - 8192)
                    continue;
                MEMORY_BASIC_INFORMATION region{};
                if (!VirtualQueryEx(process_, reinterpret_cast<void *>(a), &region, sizeof(region)) ||
                    region.State != MEM_FREE || region.RegionSize < 8192)
                    continue;
                remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, reinterpret_cast<void *>(a), 8192,
                                                                          MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
                if (remote_)
                    break;
            }
        }
        if (!remote_)
            throw std::runtime_error("Cannot allocate a nearby gem refresh adapter");
        // as with ability hooks, use adjacent RX/RW pages. copied refresh methods live at RW+512 in 256-byte slots.
        const auto copy = [&](std::uintptr_t address, const std::vector<unsigned char> &bytes)
        {
            verified_copy(process_, address, bytes.data(), bytes.size(), "Gem refresh copy failed readback");
        };
        code_ = gem_refresh_stub(before_, jobs, remote_, addon_tick);
        copy(remote_, code_);
        for (unsigned n = 0; n < jobs.size(); ++n)
        {
            const auto address = remote_ + 4608 + n * 256;
            auto method = ability_refresh_method(before_.method, address);
            const auto root = jobs[n].route - 24;
            std::memcpy(method.data() + 128, &root, 8);
            copy(address, method);
        }
        DWORD old = 0;
        if (!VirtualProtectEx(process_, reinterpret_cast<void *>(remote_), 4096, PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(process_, reinterpret_cast<void *>(remote_), code_.size()))
            throw std::runtime_error("Cannot finalize gem-refresh machine code");
        probe_gem_refresh_at(process_, found);
        for (const auto &job : jobs)
            probe_gem_route_at(process_, job.name, job.meta, job.dispatcher,
                               probe_gem_templates_at(process_, found).data_accessor_vtable, job.effect_route ? job.dispatcher + 3072 : 0);
        call_ = ability_jump_call(before_.call, remote_);
        write(process_, remote_ + 4117, static_cast<unsigned char>(0), static_cast<unsigned char>(1));
        installed_ = true;
        try
        {
            request();
            patch_ability_call(process_, pid_, before_.call, original_gem_refresh_call, call_);
        }
        catch (...)
        {
            try
            {
                detach();
            }
            catch (...)
            {
                std::cerr << "Gem refresh rollback failed. Restart NST.\n";
            }
            throw;
        }
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

GemRefreshAccess::~GemRefreshAccess()
{
    if (!process_)
        return;
    try
    {
        detach();
    }
    catch (const std::exception &error)
    {
        std::cerr << "Gem refresh restoration: " << error.what() << ". Restart NST.\n";
    }
    CloseHandle(process_);
}

void GemRefreshAccess::request(unsigned mask)
{
    // low 18 bits select six colors per campaign. the high 32 bits identify the request generation.
    if (!installed_ || (mask & ~0x3ffffu))
        throw std::runtime_error("Invalid gem refresh request");
    if (read<std::uint64_t>(process_, remote_ + 4104) != request_)
        mask |= static_cast<unsigned>(request_) & 0x3ffff;
    const auto next = ((request_ >> 32) + 1) << 32 | mask;
    write(process_, remote_ + 4096, request_, next);
    request_ = next;
}

void GemRefreshAccess::update(const GemOwnership &owned)
{
    if (!installed_ || read<std::array<unsigned char, 5>>(process_, before_.call) != call_ ||
        read<unsigned char>(process_, remote_ + 4117) != 1 || read<std::uint64_t>(process_, remote_ + 4096) != request_)
        throw std::runtime_error("Gem refresh adapter has another override");
    SIZE_T count = 0;
    std::vector<unsigned char> actual(code_.size());
    if (!ReadProcessMemory(process_, reinterpret_cast<void *>(remote_), actual.data(), actual.size(), &count) ||
        count != actual.size() || actual != code_)
        throw std::runtime_error("Gem refresh code changed externally");
    unsigned mask = 0;
    for (unsigned game = 0; game < 3; ++game)
        for (unsigned color = 0; color < 6; ++color)
            if (owned_[game][color] != owned[game][color])
                mask |= 1u << (game * 6 + color);
    if (mask)
        request(mask);
    owned_ = owned;
}

bool GemRefreshAccess::refreshed() const { return installed_ && read<std::uint64_t>(process_, remote_ + 4104) == request_; }
unsigned GemRefreshAccess::applied() const { return read<unsigned>(process_, remote_ + 4112); }

std::string GemRefreshAccess::diagnostics() const
{
    const auto counts = read<std::array<unsigned, 6>>(process_, remote_ + 4120);
    return "campaign " + std::to_string(counts[5]) + ", event-owner visits " + std::to_string(counts[0]) +
           ", matching scripts " + std::to_string(counts[1]) + ", ready " + std::to_string(counts[2]) +
           ", valid profiles " + std::to_string(counts[3]) + ", selected colors " + std::to_string(counts[4]);
}

void GemRefreshAccess::detach()
{
    if (!installed_)
        return;
    const auto current = read<std::array<unsigned char, 5>>(process_, before_.call);
    if (current != call_ && current != original_gem_refresh_call)
        throw std::runtime_error("Gem refresh call has an unknown override");
    try
    {
        const auto enabled = read<unsigned char>(process_, remote_ + 4117);
        if (enabled == 1)
            write(process_, remote_ + 4117, enabled, static_cast<unsigned char>(0));
        else if (enabled != 0)
            throw std::runtime_error("Unknown gem refresh enable flag");
    }
    catch (const std::exception &error)
    {
        std::cerr << "Gem refresh disable: " << error.what() << ".\n";
    }
    // module restoration does not depend on the level's metadata/data allocations still existing
    if (current == call_)
        patch_ability_call(process_, pid_, before_.call, call_, original_gem_refresh_call);
    installed_ = false;
}

namespace
{
    __declspec(noinline) std::set<std::uintptr_t> c3_allowed_zones(const C3PortalEntryProbe &probe,
                                                                 const std::set<std::string> &allowed)
    {
        std::set<std::uintptr_t> zones;
        for (const auto &destination : probe.destinations)
            if (allowed.count(destination.key))
                zones.insert(destination.zone);
        return zones;
    }
}
C3PortalAccess::C3PortalAccess(DWORD pid, std::uintptr_t button, std::string key)
    : button_(button), key_(std::move(key))
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for C3 portal entry gating");
    try
    {
        before_ = probe_c3_portal_entry_at(process_, button_, key_);
        if (!before_.hub_active)
            throw std::runtime_error("C3 hub is inactive");
        remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, nullptr, before_.destinations.size() * 256,
                                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!remote_)
            throw std::runtime_error("Cannot allocate C3 portal entry copies");
        const auto copied = c3_portal_entry_chain(before_, remote_);
        verified_copy(process_, remote_, copied.data(), copied.size(), "C3 portal entry copies were not verified");
        const auto current = probe_c3_portal_entry_at(process_, button_, key_);
        if (!current.hub_active || current.meta != before_.meta || current.owners != before_.owners ||
            current.entry_output != before_.entry_output || current.original_output != before_.original_output ||
            current.relic_output != before_.relic_output || current.relic_original != before_.relic_original ||
            current.relic_unlocked != before_.relic_unlocked ||
            current.destination_accessor != before_.destination_accessor || current.destinations.size() != before_.destinations.size())
            throw std::runtime_error("C3 portal graph changed during attachment");
        for (std::size_t i = 0; i < current.destinations.size(); ++i)
            if (current.destinations[i].key != before_.destinations[i].key || current.destinations[i].zone != before_.destinations[i].zone)
                throw std::runtime_error("C3 destination objects changed during attachment");
        try
        {
            write(process_, before_.entry_output, before_.original_output, remote_);
            // secret-room relic requirements should not block portal
            write(process_, before_.relic_output, before_.relic_original, before_.relic_unlocked);
        }
        catch (...)
        {
            try
            {
                restore_boss_route(process_, before_.relic_output, before_.relic_unlocked, before_.relic_original);
                restore_boss_route(process_, before_.entry_output, remote_, before_.original_output);
            }
            catch (...)
            {
                std::cerr << "C3 portal entry rollback could not be verified. Restart NST.\n";
            }
            throw;
        }
        installed_ = true;
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

C3PortalAccess::~C3PortalAccess()
{
    if (!process_)
        return;
    try
    {
        detach();
    }
    catch (const std::exception &error)
    {
        std::cerr << "C3 portal entry restoration: " << error.what() << ". Restart NST.\n";
    }
    CloseHandle(process_);
}

bool C3PortalAccess::update(const std::set<std::string> &allowed)
{
    if (!installed_)
        return false;
    const auto current = probe_c3_portal_entry_at(process_, button_, key_, remote_);
    if (!current.hub_active)
        return false;
    if (current.meta != before_.meta || current.owners != before_.owners || current.entry_output != before_.entry_output ||
        current.relic_output != before_.relic_output || current.relic_original != before_.relic_original ||
        current.relic_unlocked != before_.relic_unlocked ||
        current.original_output != before_.original_output || current.destination_accessor != before_.destination_accessor ||
        current.destinations.size() != before_.destinations.size())
        throw std::runtime_error("C3 portal graph reloaded. Rediscovery required");
    if (read<std::uintptr_t>(process_, before_.entry_output) == before_.original_output ||
        read<std::uintptr_t>(process_, before_.relic_output) == before_.relic_original)
        return false;
    for (std::size_t i = 0; i < current.destinations.size(); ++i)
    {
        if (current.destinations[i].key != before_.destinations[i].key || current.destinations[i].zone != before_.destinations[i].zone)
            throw std::runtime_error("C3 destination objects changed. Rediscovery required");
    }
    const auto allowed_zones = c3_allowed_zones(current, allowed);
    for (std::size_t i = 0; i < current.destinations.size(); ++i)
    {
        const auto branch = remote_ + i * 256 + 32;
        const auto old = read<std::uintptr_t>(process_, branch);
        if (old && old != before_.original_output)
            throw std::runtime_error("C3 portal entry has an unknown branch");
        // poaired buttons share a zone. their individual trigger gates still enforce each entrance's access.
        const auto desired = allowed_zones.count(current.destinations[i].zone) ? before_.original_output : 0;
        if (old != desired)
            write(process_, branch, old, desired);
    }
    return true;
}

void C3PortalAccess::detach()
{
    if (!installed_)
        return;
    const auto current = probe_c3_portal_entry_at(process_, button_, key_, remote_);
    if (!current.hub_active || current.meta != before_.meta || current.owners != before_.owners ||
        current.relic_output != before_.relic_output || current.relic_original != before_.relic_original ||
        current.relic_unlocked != before_.relic_unlocked ||
        current.entry_output != before_.entry_output || current.original_output != before_.original_output)
        throw std::runtime_error("C3 hub is inactive or portal graph changed before restoration");
    restore_boss_route(process_, before_.relic_output, before_.relic_unlocked, before_.relic_original);
    restore_boss_route(process_, before_.entry_output, remote_, before_.original_output);
    installed_ = false;
}

PortalTriggerGate::PortalTriggerGate(DWORD pid, std::string level, std::uintptr_t entity, C3TriggerTypes types)
    : level_(std::move(level)), entity_(entity), c3_types_(types)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                               PROCESS_VM_READ | PROCESS_VM_WRITE,
                           FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open verified NST process for gate writes (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    try
    {
        original_ = state().data;
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

PortalTriggerGate::~PortalTriggerGate()
{
    if (!process_)
        return;
    if (clone_)
    {
        try
        {
            if (!restore())
                std::cerr << level_ << " gate: hub inactive at shutdown. Copied data remains until game exit.\n";
        }
        catch (const std::exception &error)
        {
            std::cerr << level_ << " gate: restoration failed: " << error.what() << '\n';
        }
    }
    CloseHandle(process_);
}

C2PortalProbe PortalTriggerGate::state() const
{
    return level_.size() >= 4 && level_[1] == '3' ? probe_c3_button_at(process_, entity_, level_, c3_types_, clone_) : probe_c2_portal_at(process_, entity_, level_, clone_);
}

bool PortalTriggerGate::verify_data(std::uintptr_t address, bool events_enabled) const
{
    return level_.size() >= 4 && level_[1] == '3' ? verify_c3_trigger_data(process_, address, c3_types_, events_enabled) : verify_c2_trigger_data(process_, address, level_, events_enabled);
}

bool PortalTriggerGate::set_locked(bool locked)
{
    const auto portal = state();
    if (!portal.hub_active)
        return false;
    if (portal.data != clone_)
        original_ = portal.data;
    if (locked && portal.data != clone_)
    {
        std::vector<unsigned char> copied;
        if (level_[1] == '3')
        {
            const auto source = read<std::array<unsigned char, 88>>(process_, original_);
            copied.assign(source.begin(), source.end());
        }
        else
        {
            const auto source = read<std::array<unsigned char, 112>>(process_, original_);
            copied.assign(source.begin(), source.end());
        }
        if (!verify_data(original_, true))
            throw std::runtime_error(level_ + " original trigger data changed");
        // a reload can reuse a source address with new query-filter pointers. ignore only reference counts.
        const bool same_source = copy_source_ == original_ && source_bytes_.size() == copied.size() &&
                                 std::equal(copied.begin(), copied.begin() + 8, source_bytes_.begin()) &&
                                 std::equal(copied.begin() + 16, copied.end(), source_bytes_.begin() + 16);
        if (!clone_ || !same_source)
        {
            source_bytes_ = copied;
            copied = trigger_data_copy(std::move(copied));
            clone_ = reinterpret_cast<std::uintptr_t>(
                VirtualAllocEx(process_, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            if (!clone_)
                throw std::runtime_error("Cannot allocate NST trigger copy");
            SIZE_T count = 0;
            if (!WriteProcessMemory(process_, reinterpret_cast<void *>(clone_), copied.data(),
                                    copied.size(), &count) ||
                count != copied.size() ||
                !verify_data(clone_, false))
                throw std::runtime_error(level_ + " trigger copy was not verified");
            copy_source_ = original_;
        }
        if (!verify_data(clone_, false))
            throw std::runtime_error(level_ + " trigger copy changed");
        const auto current = state();
        if (!current.hub_active || current.component != portal.component || current.data != original_)
            throw std::runtime_error(level_ + " portal changed before locking");
        write(process_, portal.component + 16, original_, clone_);
    }
    else if (!locked && portal.data == clone_)
    {
        if (!original_ || !verify_data(original_, true))
            throw std::runtime_error("Cannot verify original NST trigger for restoration");
        const auto current = state();
        if (!current.hub_active || current.component != portal.component || current.data != clone_)
            throw std::runtime_error(level_ + " portal changed before restoration");
        write(process_, portal.component + 16, clone_, original_);
    }
    return true;
}

C2LiftTraversal::C2LiftTraversal(DWORD pid, unsigned floor, std::uintptr_t entity)
    : floor_(floor), entity_(entity)
{
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                               PROCESS_VM_READ | PROCESS_VM_WRITE,
                           FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for lift writes (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    try
    {
        probe_c2_lift_at(process_, entity_, floor_);
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

C2LiftTraversal::~C2LiftTraversal()
{
    if (!process_)
        return;
    if (overridden_)
    {
        try
        {
            if (!restore())
                std::cerr << "Lift " << floor_ << ": hub inactive at shutdown. Restart clears temporary data override.\n";
        }
        catch (const std::exception &error)
        {
            std::cerr << "Lift " << floor_ << " restoration: " << error.what() << '\n';
        }
    }
    CloseHandle(process_);
}

bool C2LiftTraversal::set_enabled(bool enabled)
{
    const auto lift = probe_c2_lift_at(process_, entity_, floor_);
    if (!lift.hub_active)
        return false;
    if (!overridden_)
    {
        if (!enabled)
            return true;
        source_data_ = read<std::array<unsigned char, 232>>(process_, lift.data);
        if (read<std::uintptr_t>(process_, lift.data + 24) != lift.data + 40)
            throw std::runtime_error("Lift data is not a private engine object. Override refused");
        original_ = lift.data;
    }
    const auto current = probe_c2_lift_at(process_, entity_, floor_);
    if (!current.hub_active || current.component != lift.component || current.data != original_ ||
        !matches_lift_data(process_, original_, source_data_, floor_))
        throw std::runtime_error("Lift component data changed before traversal override");
    const auto target = enabled ? c2_lift_data_override(source_data_, floor_) : source_data_;
    overridden_ = true;
    for (unsigned direction = 0; direction < 2; ++direction)
        if (c2_lift_direction_allowed(floor_, direction))
        {
            const unsigned offset = direction ? 120 : 168;
            std::uintptr_t desired = 0;
            std::memcpy(&desired, target.data() + offset, 8);
            const auto before = read<std::uintptr_t>(process_, original_ + offset);
            std::uintptr_t saved = 0;
            std::memcpy(&saved, source_data_.data() + offset, 8);
            if (before && before != saved)
                throw std::runtime_error("Lift requirement changed before write");
            if (before != desired)
                write(process_, original_ + offset, before, desired);
        }
    if (!enabled)
        overridden_ = false;
    return true;
}

C2BossPortals::C2BossPortals(DWORD pid, const std::map<std::string, std::uintptr_t> &entities)
{
    if (entities.empty() || entities.size() > 5)
        throw std::invalid_argument("Expected one to five Crash 2 bosses");
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                               PROCESS_VM_READ | PROCESS_VM_WRITE,
                           FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for boss events");
    try
    {
        std::vector<C2BossEventProbe> probes;
        for (const auto &[level, entity] : entities)
        {
            const auto probe = probe_c2_boss_events_at(process_, entity, level);
            if (!probe.portal.hub_active)
                throw std::runtime_error(level + " must be active before attaching its event route");
            if (!probes.empty() && (probe.entry_output != probes[0].entry_output ||
                                    probe.original_output != probes[0].original_output || probe.owner_accessor != probes[0].owner_accessor))
                throw std::runtime_error("Boss portals do not share the verified entry graph");
            entries_.emplace(level, Entry{probe});
            probes.push_back(probe);
        }
        remote_ = reinterpret_cast<std::uintptr_t>(
            VirtualAllocEx(process_, nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!remote_)
            throw std::runtime_error("Cannot allocate boss script-event copy");
        const auto graph = c2_boss_event_chain(probes, remote_);
        std::size_t offset = 0;
        for (auto &[level, entry] : entries_)
        {
            entry.remote = remote_ + offset;
            std::memcpy(&entry.branch, graph.data() + offset + 32, sizeof(entry.branch));
            offset += 512;
        }
        SIZE_T count = 0;
        if (!WriteProcessMemory(process_, reinterpret_cast<void *>(remote_), graph.data(), graph.size(), &count) ||
            count != graph.size())
            throw std::runtime_error("Boss script-event copy was not verified");
        offset = 0;
        for (const auto &[level, entry] : entries_)
        {
            const auto copied = read<std::array<unsigned char, 512>>(process_, entry.remote);
            if (!std::equal(copied.begin(), copied.end(), graph.begin() + offset))
                throw std::runtime_error(level + " script-event copy was not verified");
            offset += 512;
            const auto &before = entry.before;
            const auto &portal = before.portal;
            const auto current = probe_c2_boss_events_at(process_, portal.entity, level);
            if (!current.portal.hub_active || current.entry_output != before.entry_output ||
                current.portal.entry_component != portal.entry_component ||
                current.portal.entry_gate != portal.entry_gate || current.portal.boss_data != portal.boss_data ||
                current.portal.entry_allowed != portal.entry_allowed || current.portal.effects != portal.effects)
                throw std::runtime_error(level + " changed before installing its event route");
        }
        const auto &first = entries_.begin()->second.before;
        write(process_, first.entry_output, first.original_output, remote_);
        installed_ = true;
    }
    catch (...)
    {
        // a write can succeed while its verification fails
        if (remote_ && !entries_.empty())
        {
            try
            {
                const auto &first = entries_.begin()->second.before;
                if (read<std::uintptr_t>(process_, first.entry_output) == remote_)
                    write(process_, first.entry_output, remote_, first.original_output);
            }
            catch (const std::exception &error)
            {
                std::cerr << "Boss setup restoration failed: " << error.what() << ". Restart NST.\n";
            }
        }
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

C2BossPortals::~C2BossPortals()
{
    if (!process_)
        return;
    if (installed_)
    {
        try
        {
            detach();
        }
        catch (const std::exception &error)
        {
            std::cerr << "Boss route restoration: " << error.what() << ". Restart NST.\n";
        }
    }
    CloseHandle(process_);
}

C2PortalProbe C2BossPortals::state(const std::string &level) const
{
    const auto &before = entries_.at(level).before.portal;
    const auto current = probe_c2_portal_at(process_, before.entity, level);
    if (current.hub_active && (current.entry_component != before.entry_component ||
                               current.entry_meta != before.entry_meta || current.entry_gate != before.entry_gate ||
                               current.boss_data != before.boss_data))
        throw std::runtime_error(level + " component changed. Reattach its event route");
    return current;
}

void C2BossPortals::set_branch(const std::string &level, std::uintptr_t branch)
{
    auto &entry = entries_.at(level);
    const auto &before = entry.before;
    const auto next = std::next(entries_.find(level));
    const auto fallback = next == entries_.end() ? before.original_output : next->second.remote;
    if (!installed_ || read<std::uintptr_t>(process_, before.entry_output) != remote_ ||
        read<std::uintptr_t>(process_, entry.remote + 16) != before.owner_accessor ||
        read<std::uintptr_t>(process_, entry.remote + 40) != fallback ||
        read<std::uintptr_t>(process_, entry.remote + 136) != before.portal.entity)
        throw std::runtime_error("Boss event route changed");
    if (branch != entry.branch)
        write(process_, entry.remote + 32, entry.branch, branch);
    entry.branch = branch;
}

bool C2BossPortals::update()
{
    if (!installed_)
        return false;
    const auto &first = entries_.begin()->second.before;
    if (read<std::uintptr_t>(process_, first.entry_output) == first.original_output)
        return false;
    for (const auto &[level, entry] : entries_)
    {
        const auto current = state(level);
        if (!current.hub_active)
            return false;
        // preserve game teleport locks once enabled
        set_branch(level, current.entry_allowed ? entry.before.original_output : entry.remote + 160);
    }
    return true;
}

bool C2BossPortals::matches(const std::map<std::string, std::uintptr_t> &entities) const
{
    if (entries_.size() != entities.size())
        return false;
    for (const auto &[level, entity] : entities)
    {
        const auto entry = entries_.find(level);
        if (entry == entries_.end() || entry->second.before.portal.entity != entity)
            return false;
    }
    return true;
}

void C2BossPortals::detach()
{
    // remove the route first. an in-flight game script event still has valid retained objects
    if (installed_)
    {
        const auto &first = entries_.begin()->second.before;
        restore_boss_route(process_, first.entry_output, remote_, first.original_output);
        installed_ = false;
    }
}

C3HubAccess::C3HubAccess(DWORD pid, const std::map<std::string, std::uintptr_t> &entities)
{
    if (entities.empty() || entities.size() > 36)
        throw std::invalid_argument("Expected one to 36 C3 buttons");
    process_ = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
    if (!process_)
        throw std::runtime_error("Cannot open NST for C3 hub access (Windows error " + std::to_string(GetLastError()) + ")");
    try
    {
        std::vector<C3ButtonEventProbe> probes;
        for (const auto &[key, entity] : entities)
        {
            const auto probe = probe_c3_button_events_at(process_, entity, key);
            entries_.emplace(key, probe);
            probes.push_back(probe);
        }
        const auto &anchor = *entities.begin();
        barriers_ = probe_c3_barriers_at(process_, anchor.second, anchor.first);
        remote_ = reinterpret_cast<std::uintptr_t>(VirtualAllocEx(process_, nullptr, probes.size() * 512 + 4 * 256,
                                                                  MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!remote_)
            throw std::runtime_error("Cannot allocate C3 script-event copies");
        auto copied = c3_button_event_chain(probes, remote_);

        barrier_route_ = remote_ + copied.size();
        const auto gates = c3_barrier_event_chain(probes.front(), barriers_, barrier_route_);
        copied.insert(copied.end(), gates.begin(), gates.end());
        verified_copy(process_, remote_, copied.data(), copied.size(), "C3 script-event copies were not verified");
        // validate the whole owner set again before installing the shared route
        for (const auto &[key, probe] : entries_)
            if (probe_c3_button_events_at(process_, probe.entity, key).meta != probe.meta)
                throw std::runtime_error("C3 hub reloaded during hub-access hook installation");
        const auto &first = probes.front();
        // activation/visibility share the owner-test chain. contact uses its parallel chain at +320.
        try
        {
            write(process_, first.entry_output, first.original_output, remote_);
            write(process_, first.contact_output, first.original_contact, remote_ + 320);
            for (unsigned i = 0; i < 2; ++i)
                write(process_, first.visibility_outputs[i], first.original_visibility[i], remote_);
        }
        catch (...)
        {
            try
            {
                for (unsigned i = 0; i < 2; ++i)
                    restore_boss_route(process_, first.visibility_outputs[i], remote_, first.original_visibility[i]);
                restore_boss_route(process_, first.contact_output, remote_ + 320, first.original_contact);
                restore_boss_route(process_, first.entry_output, remote_, first.original_output);
            }
            catch (...)
            {
                std::cerr << "C3 activation route installation failed and rollback could not be verified. Restart NST.\n";
            }
            throw;
        }
        installed_ = true;
    }
    catch (...)
    {
        CloseHandle(process_);
        process_ = nullptr;
        throw;
    }
}

C3HubAccess::~C3HubAccess()
{
    if (!process_)
        return;
    try
    {
        detach();
    }
    catch (const std::exception &error)
    {
        std::cerr << "C3 hub access restoration: " << error.what() << ". Restart NST.\n";
    }
    CloseHandle(process_);
}

bool C3HubAccess::matches(const std::map<std::string, std::uintptr_t> &entities) const
{
    if (entities.size() != entries_.size())
        return false;
    for (const auto &[key, entity] : entities)
    {
        const auto found = entries_.find(key);
        if (found == entries_.end() || found->second.entity != entity)
            return false;
    }
    return true;
}

bool C3HubAccess::update(const std::set<std::string> &allowed, bool traversal, const std::array<bool, 4> &areas)
{
    if (!installed_)
        return false;
    const auto &first = entries_.begin()->second;
    if (read<std::uintptr_t>(process_, first.entry_output) == first.original_output ||
        read<std::uintptr_t>(process_, first.contact_output) == first.original_contact)
        return false;
    for (const auto &[key, before] : entries_)
    {
        const auto current = probe_c3_button_events_at(process_, before.entity, key, remote_);
        if (current.script != before.script || current.meta != before.meta || current.entry_output != before.entry_output ||
            current.original_output != before.original_output || current.contact_output != before.contact_output ||
            current.original_contact != before.original_contact)
            throw std::runtime_error("C3 hub access owner/graph reloaded. Rediscovery required");
    }
    const auto &anchor = *entries_.begin();
    const auto barriers = probe_c3_barriers_at(process_, anchor.second.entity, anchor.first,
                                               traversal_installed_ ? barrier_route_ : 0);
    for (unsigned i = 0; i < barriers.size(); ++i)
        if (barriers[i].entity != barriers_[i].entity || barriers[i].meta != barriers_[i].meta ||
            barriers[i].entry_output != barriers_[i].entry_output || barriers[i].disable != barriers_[i].disable)
            throw std::runtime_error("C3 traversal script graph reloaded. Rediscovery required");
    // per-button +32 selects enable (+160) or hide (+384). the parallel contact test invokes activation (+240).
    unsigned i = 0;
    for (const auto &[key, probe] : entries_)
    {
        const auto branch = remote_ + i++ * 512 + 32;
        const auto old = read<std::uintptr_t>(process_, branch);
        const auto enable = branch - 32 + 160;
        const auto hide = branch - 32 + 384;
        if (old != hide && old != enable)
            throw std::runtime_error("C3 access dispatcher has an unknown branch");
        const auto desired = allowed.count(key) ? enable : hide;
        if (old != desired)
            write(process_, branch, old, desired);
        const auto contact_branch = branch + 320;
        const auto contact_old = read<std::uintptr_t>(process_, contact_branch);
        const auto activate = branch - 32 + 240;
        if (contact_old && contact_old != activate)
            throw std::runtime_error("C3 contact dispatcher has an unknown branch");
        const auto contact_desired = allowed.count(key) ? activate : 0;
        if (contact_old != contact_desired)
        {
            write(process_, contact_branch, contact_old, contact_desired);
            std::cout << "Crash 3 " << key << " button contact "
                      << (contact_desired ? "enabled" : "blocked") << ".\n"
                      << std::flush;
        }
    }
    for (unsigned i = 0; i < barriers_.size(); ++i)
    {
        const auto branch = barrier_route_ + i * 256 + 32;
        const auto before = read<std::uintptr_t>(process_, branch);
        if (before && before != barriers_[i].disable)
            throw std::runtime_error("C3 area dispatcher has an unknown branch");
        const auto desired = areas[i] ? barriers_[i].disable : 0;
        if (before != desired)
            write(process_, branch, before, desired);
    }
    if (traversal != traversal_installed_)
    {
        const auto &gate = barriers_[0];
        const auto before = traversal_installed_ ? barrier_route_ : gate.original_output;
        const auto after = traversal ? barrier_route_ : gate.original_output;
        try
        {
            write(process_, gate.entry_output, before, after);
        }
        catch (...)
        {
            try
            {
                restore_boss_route(process_, gate.entry_output, after, before);
            }
            catch (...)
            {
                std::cerr << "C3 barrier route rollback could not be verified. Restart NST.\n";
            }
            throw;
        }
        traversal_installed_ = traversal;
    }
    return true;
}

void C3HubAccess::detach()
{
    if (!installed_ && !traversal_installed_)
        return;
    const auto &anchor = *entries_.begin();
    // revalidate current owners/graphs first. dont restore into freed or reloaded asset memory
    for (const auto &[key, before] : entries_)
    {
        const auto current = probe_c3_button_events_at(process_, before.entity, key, remote_);
        if (current.meta != before.meta || current.script != before.script || current.entry_output != before.entry_output)
            throw std::runtime_error("C3 graph changed before restoration");
    }
    const auto barriers = probe_c3_barriers_at(process_, anchor.second.entity, anchor.first,
                                               traversal_installed_ ? barrier_route_ : 0);
    for (unsigned i = 0; i < barriers.size(); ++i)
        if (barriers[i].entity != barriers_[i].entity || barriers[i].meta != barriers_[i].meta ||
            barriers[i].entry_output != barriers_[i].entry_output || barriers[i].disable != barriers_[i].disable)
            throw std::runtime_error("C3 barrier graph changed before restoration");
    if (traversal_installed_)
    {
        const auto &gate = barriers_[0];
        restore_boss_route(process_, gate.entry_output, barrier_route_, gate.original_output);
        traversal_installed_ = false;
    }
    if (installed_)
    {
        const auto &first = anchor.second;
        for (unsigned i = 0; i < 2; ++i)
            restore_boss_route(process_, first.visibility_outputs[i], remote_, first.original_visibility[i]);
        restore_boss_route(process_, first.contact_output, remote_ + 320, first.original_contact);
        restore_boss_route(process_, first.entry_output, remote_, first.original_output);
        installed_ = false;
    }
}
