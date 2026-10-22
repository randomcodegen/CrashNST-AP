#pragma once

#include <windows.h>
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <set>
#include "game_memory.h"

struct C2LiftProbe;
void patch_relative_call(HANDLE process, DWORD pid, std::uintptr_t site,
                       const std::array<unsigned char, 5> &expected, const std::array<unsigned char, 5> &replacement);
inline constexpr std::array<std::pair<const char *, const char *>, 4> secret_stages{{{"L226", "L215"}, {"L227", "L216"}, {"L331", "L314"}, {"L332", "L311"}}};
// vanilla SecretPortal_ID order: alternate sections, then the two dedicated secret stages.
inline constexpr std::array<std::pair<const char *, const char *>, 5> c2_secret_entrances{{
    {"L207", "L213"}, {"L202", "L207"}, {"L214", "L217"}, {"L226", "L215"}, {"L227", "L216"}}};
class LevelRouting
{
public:
    LevelRouting(DWORD pid, const std::map<std::string, std::string> &placements);
    ~LevelRouting();
    void detach();
    std::uintptr_t state() const { return remote_ + 4096; }
    bool bind_secret(const std::array<unsigned char, 48> &handle);
    bool ready() const { return variable_ != 0; }
    unsigned start_count() const;
    unsigned secret_return_count() const;
    void secrets(const std::set<std::string> &allowed);
    std::string last_entrance() const;
    void resume_entrances(const std::map<char, std::string> &entrances);

private:
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    std::uintptr_t base_ = 0, remote_ = 0, variable_ = 0;
    std::vector<ZoneRecord> zones_;
    std::map<std::string, std::uintptr_t> entrances_;
    std::array<std::array<unsigned char, 5>, 8> calls_{};
    unsigned installed_ = 0;
};

struct CrateBreak
{
    std::string zone, name;
    std::array<unsigned char, 12> position{};
};
class CrateEvents
{
public:
    CrateEvents(DWORD pid, std::uintptr_t meta, std::uintptr_t identity = 0);
    ~CrateEvents();
    std::vector<CrateBreak> drain();
    void detach();

private:
    HANDLE process_ = nullptr;
    CrateProbe before_;
    std::uintptr_t remote_ = 0, queue_ = 0;
    std::uint64_t consumed_ = 0;
    bool installed_ = false;
};

class PortalTriggerGate
{
public:
    PortalTriggerGate(DWORD pid, std::string level, std::uintptr_t entity, C3TriggerTypes types = {});
    ~PortalTriggerGate();
    PortalTriggerGate(const PortalTriggerGate &) = delete;
    PortalTriggerGate &operator=(const PortalTriggerGate &) = delete;

    // returns false while the hub component is inactive
    bool set_locked(bool locked);
    C2PortalProbe state() const;
    bool restore() { return set_locked(false); }
    std::uintptr_t entity() const { return entity_; }

private:
    std::string level_;
    HANDLE process_ = nullptr;
    std::uintptr_t entity_ = 0;
    std::uintptr_t original_ = 0;
    std::uintptr_t clone_ = 0;
    std::uintptr_t copy_source_ = 0;
    std::vector<unsigned char> source_bytes_;
    C3TriggerTypes c3_types_;
    bool verify_data(std::uintptr_t address, bool events_enabled) const;
};
using C2PortalGate = PortalTriggerGate;

// check the current destination in the portal entry script event
class C3PortalAccess
{
public:
    C3PortalAccess(DWORD pid, std::uintptr_t button, std::string key);
    ~C3PortalAccess();
    C3PortalAccess(const C3PortalAccess &) = delete;
    C3PortalAccess &operator=(const C3PortalAccess &) = delete;
    bool update(const std::set<std::string> &allowed);
    void detach();

private:
    HANDLE process_ = nullptr;
    std::uintptr_t button_ = 0, remote_ = 0;
    std::string key_;
    C3PortalEntryProbe before_;
    bool installed_ = false;
};

// stop only secret-transfer script branches, before their grab/teleport messages
class C1MapAccess
{
public:
    C1MapAccess(DWORD pid, C1MapDiscovery found);
    ~C1MapAccess();
    C1MapAccess(const C1MapAccess &) = delete;
    C1MapAccess &operator=(const C1MapAccess &) = delete;
    bool update(const std::set<std::string> &allowed, bool traversal);
    void detach();

private:
    HANDLE process_ = nullptr;
    C1MapProbe before_;
    std::uintptr_t remote_ = 0, secret_ = 0;
    bool installed_ = false, traversal_ = false;
};

class AbilityAccess
{
public:
    AbilityAccess(DWORD pid, const GemDiscovery &found, const AbilityOwnership &owned, std::uintptr_t jump_filter = 0);
    ~AbilityAccess();
    AbilityAccess(const AbilityAccess &) = delete;
    AbilityAccess &operator=(const AbilityAccess &) = delete;
    // false means the compiled graph unloaded/changed
    bool update(const AbilityOwnership &owned);
    void detach();
    std::uintptr_t meta() const { return before_.meta; }
    bool refreshed() const;
    std::uintptr_t menu_meta() const { return menu_.meta; }
    std::uintptr_t hud_meta() const { return filler_.meta; }
    std::uintptr_t jump_filter() const { return jump_filter_; }
    bool filler_ready() const;
    bool filler_pending() const;
    void queue_filler(unsigned index, std::int64_t item, std::uintptr_t zone);

private:
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    GemDiscovery found_;
    AbilityProbe before_;
    AbilityMenuProbe menu_;
    FillerProbe filler_;
    GemTemplates menu_templates_;
    AbilityOwnership owned_{};
    std::uintptr_t jump_filter_ = 0;
    std::uintptr_t remote_ = 0;
    std::uintptr_t menu_remote_ = 0;
    std::array<unsigned char, 5> call_{};
    std::array<unsigned char, 5> refresh_call_{};
    unsigned generation_ = 1;
    unsigned filler_amount_ = 10;
    bool installed_ = false;
};

class GemRouteAccess
{
public:
    GemRouteAccess(DWORD pid, const GemDiscovery &found, const std::string &name,
                   const GemOwnership &owned);
    ~GemRouteAccess();
    GemRouteAccess(const GemRouteAccess &) = delete;
    GemRouteAccess &operator=(const GemRouteAccess &) = delete;
    void update(const GemOwnership &owned);
    void restore_query(); // keep the duplicate-effect guard until game gem-cache refresh finishes
    void detach();
    std::uintptr_t meta() const { return before_.meta; }
    GemRouteProbe refresh_job() const
    {
        auto job = before_;
        job.dispatcher = remote_;
        return job;
    }

private:
    HANDLE process_ = nullptr;
    GemDiscovery found_;
    GemRouteProbe before_;
    GemTemplates templates_;
    GemOwnership owned_{};
    std::uintptr_t remote_ = 0;
    bool installed_ = false;
};

// one independent game-thread call
class GemRefreshAccess
{
public:
    GemRefreshAccess(DWORD pid, const GemDiscovery &found, std::vector<GemRouteProbe> jobs,
                     const GemOwnership &owned, std::uintptr_t addon_tick = 0);
    ~GemRefreshAccess();
    GemRefreshAccess(const GemRefreshAccess &) = delete;
    GemRefreshAccess &operator=(const GemRefreshAccess &) = delete;
    void update(const GemOwnership &owned);
    void request(unsigned mask = 0x3ffff);
    bool refreshed() const;
    unsigned applied() const;
    std::string diagnostics() const;
    void detach();

private:
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
    GemRefreshProbe before_;
    GemOwnership owned_{};
    std::uintptr_t remote_ = 0;
    std::uint64_t request_ = 0;
    std::array<unsigned char, 5> call_{};
    std::vector<unsigned char> code_;
    bool installed_ = false;
};

class C2LiftTraversal
{
public:
    C2LiftTraversal(DWORD pid, unsigned floor, std::uintptr_t entity);
    ~C2LiftTraversal();
    C2LiftTraversal(const C2LiftTraversal &) = delete;
    C2LiftTraversal &operator=(const C2LiftTraversal &) = delete;
    bool set_enabled(bool enabled);
    bool restore() { return set_enabled(false); }
    std::uintptr_t entity() const { return entity_; }

private:
    HANDLE process_ = nullptr;
    unsigned floor_;
    std::uintptr_t entity_;
    std::uintptr_t original_ = 0;
    bool overridden_ = false;
    std::array<unsigned char, 232> source_data_{};
};

// one shared OnCEnter route selects owned bosses by entity
class C2BossPortals
{
public:
    C2BossPortals(DWORD pid, const std::map<std::string, std::uintptr_t> &entities);
    ~C2BossPortals();
    C2BossPortals(const C2BossPortals &) = delete;
    C2BossPortals &operator=(const C2BossPortals &) = delete;
    // AP lifecycle: false requests reattachment after hub/graph reload
    bool update();
    bool matches(const std::map<std::string, std::uintptr_t> &entities) const;

private:
    HANDLE process_ = nullptr;
    struct Entry
    {
        C2BossEventProbe before;
        std::uintptr_t remote = 0;
        std::uintptr_t branch = 0;
    };
    std::map<std::string, Entry> entries_;
    std::uintptr_t remote_ = 0;
    bool installed_ = false;
    C2PortalProbe state(const std::string &level) const;
    void detach();
    void set_branch(const std::string &level, std::uintptr_t branch);
};

// button-enable script plus barrier contact removal
class C3HubAccess
{
public:
    C3HubAccess(DWORD pid, const std::map<std::string, std::uintptr_t> &entities);
    ~C3HubAccess();
    C3HubAccess(const C3HubAccess &) = delete;
    C3HubAccess &operator=(const C3HubAccess &) = delete;
    bool update(const std::set<std::string> &allowed, bool traversal, const std::array<bool, 4> &areas);
    bool matches(const std::map<std::string, std::uintptr_t> &entities) const;
    void detach();

private:
    HANDLE process_ = nullptr;
    std::map<std::string, C3ButtonEventProbe> entries_;
    std::array<C3BarrierEventProbe, 4> barriers_{};
    std::uintptr_t remote_ = 0;
    bool installed_ = false, traversal_installed_ = false;
    std::uintptr_t barrier_route_ = 0;
};
