#pragma once
#include "check_progress.h"
#include "hub_access.h"
#include <json/json.h>

inline constexpr int seed_protocol = 5;
using CheckKey = std::pair<std::string, std::string>;
struct CampaignGoals
{
    std::set<std::string> levels, regular_levels;
    std::string type = "cortex";
    unsigned required = 0;
    std::map<std::string, unsigned> requirements;
};
struct SeedConfig
{
    explicit SeedConfig(const Json::Value &data);
    std::map<CheckKey, std::int64_t> checks;
    std::set<std::string> levels;
    std::map<std::string, std::string> placements;
    int shuffle = 0;
    bool hub_area_locks = false, level_access_locks = true;
    CampaignGoals goals;
    CheckRules rules;
    std::map<std::string, unsigned> movement_styles;
};
std::int64_t access_item_id(const std::string &level);
bool has_access(const std::vector<std::pair<std::int64_t, int>> &received, const std::string &level);
bool level_locked(const SeedConfig &seed, const std::vector<std::pair<std::int64_t, int>> &received,
                  const std::string &level, const std::set<std::int64_t> &cleared = {});
bool level_accessible(const SeedConfig &seed, const std::vector<std::pair<std::int64_t, int>> &received,
                      const std::string &level, const std::set<std::int64_t> &cleared = {});
bool hub_area_unlocked(const SeedConfig &seed, const std::vector<std::pair<std::int64_t, int>> &received,
                       unsigned campaign, unsigned area);
unsigned goal_progress(const CampaignGoals &goals, const std::set<std::string> &completed,
                       const std::map<CheckKey, std::int64_t> &checks, const std::set<std::int64_t> &acknowledged,
                       const std::vector<std::pair<std::int64_t, int>> &received);
bool goals_complete(const CampaignGoals &goals, const std::set<std::string> &completed,
                    const std::map<CheckKey, std::int64_t> &checks, const std::set<std::int64_t> &acknowledged,
                    const std::vector<std::pair<std::int64_t, int>> &received);
