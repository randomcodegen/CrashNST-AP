#pragma once
#include <windows.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "check_progress.h"
#include "ingame_protocol.h"

struct BoxMarker
{
    std::string level;
    std::array<float, 3> position;
    std::string name;
    unsigned color = 0;
};
class BoxMarkers
{
public:
    explicit BoxMarkers(DWORD pid, bool startup = false, unsigned marker_mode = ingame::MarkersTint);
    ~BoxMarkers();
    void update(std::vector<BoxMarker> markers);
    void message(std::string text);
    void goal(const std::string &text);
    // unlocked ordinary destinations and independent Sxxx secret portal keys
    void progress(const CheckProgress &counts, const std::map<std::string, std::string> &placements,
                  const std::set<std::string> &unlocked);
    void feed(ingame::Line line, bool popup = true);
    void completion(const std::vector<std::string> &items, const std::vector<std::string> &locations);
    void configure(const std::map<std::string, unsigned> &styles, bool death_link);
    void c3_buttons(bool ready, const std::set<std::string> &allowed = {});
    unsigned collectibles(std::vector<ingame::Collectible> config);
    std::vector<ingame::CollectibleObservation> collectible_observations();
    void connection(const std::string &text);
    std::vector<std::string> commands();
    void receive_death();
    unsigned deaths();
    std::uintptr_t tick() const { return tick_; }
    std::uintptr_t double_jump_filter() const { return double_jump_filter_; }
    std::uintptr_t crate_identity() const { return crate_identity_; }
    bool rendering();
    bool startup() const { return startup_; }
    void startup_popup(bool show);
    void marker_mode(unsigned mode);
    void hub_distance(const std::array<unsigned, 3> &distances);
    void tint_opacity(float opacity);
    bool toggle(const std::string &target, const std::string &value);
    void stop(bool handover = false);

private:
    void lock();
    void unlock();
    HANDLE mapping_ = nullptr, mutex_ = nullptr;
    ingame::Shared *shared_ = nullptr;
    std::uintptr_t tick_ = 0;
    std::uintptr_t double_jump_filter_ = 0;
    std::uintptr_t crate_identity_ = 0;
    bool owned_ = false;
    bool startup_ = false;
    DWORD pid_ = 0;
    std::uintptr_t movement_site_ = 0;
    std::array<unsigned char, 5> movement_call_{};
};
