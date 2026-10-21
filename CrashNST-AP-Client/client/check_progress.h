#pragma once
#include <json/json.h>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <array>

unsigned box_marker_color(bool spoilers, const Json::Value &location_flags, std::int64_t location);
struct CheckCounts
{
    unsigned done = 0, in_logic = 0, total = 0;
    bool logic_known = true;
    std::string text() const;
};
struct LevelProgress : CheckCounts
{
    std::string title;
    std::array<CheckCounts, 3> collectibles{};
    std::string collectible_text() const;
};
struct CheckProgress
{
    std::map<std::string, LevelProgress> levels;
    std::set<std::int64_t> reachable;
    std::string error;
};
struct CheckRules
{
    struct Rule
    {
        std::pair<std::string, std::string> key;
        std::int64_t id;
        std::string title;
        std::map<std::int64_t, unsigned> required;
        bool known;
    };
    std::vector<Rule> checks;
    std::map<std::int64_t, std::int64_t> local_reward_items;
    std::map<std::string, std::set<std::int64_t>> boss_requirements;
    std::set<std::string> gated_finals;
    std::set<std::int64_t> regular_clears;
    std::map<std::string, unsigned> cortex_requirements;
    std::string error;
};
unsigned requirement_progress(const CheckRules &rules, const std::string &kind,
                              const std::vector<std::pair<std::int64_t, int>> &items,
                              const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending = {});
bool cortex_requirements_met(const CheckRules &rules, const std::vector<std::pair<std::int64_t, int>> &items,
                             const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending = {});
CheckRules compile_check_rules(const std::map<std::pair<std::string, std::string>, std::int64_t> &checks,
                               const Json::Value &slot_data, const std::set<std::string> &selected);
CheckProgress check_progress(const CheckRules &rules, const std::vector<std::pair<std::int64_t, int>> &items,
                             const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending,
                             const std::set<std::int64_t> &local_collected = {});
std::set<std::int64_t> box_marker_ids(const CheckProgress &progress,
                                      const std::map<std::pair<std::string, std::string>, std::int64_t> &checks,
                                      const std::set<std::int64_t> &checked, const std::set<std::int64_t> &pending,
                                      bool out_of_logic);
std::array<LevelProgress, 3> campaign_progress(const CheckProgress &progress,
                                               const std::map<std::string, std::string> &placements);
