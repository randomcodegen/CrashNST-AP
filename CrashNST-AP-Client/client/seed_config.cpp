#include "seed_config.h"
#include "crate_catalogue.h"
#include "game_hooks.h"
#include "collectibles.h"
#include <algorithm>
#include <stdexcept>
std::int64_t access_item_id(const std::string &level)
{
    if (level.size() != 4 || (level[0] != 'L' && level[0] != 'B') || level[1] < '1' || level[1] > '3' ||
        !std::all_of(level.begin() + 2, level.end(), [](char digit) { return digit >= '0' && digit <= '9'; }))
        return 0;
    return 0xCB0000 + (level[0] == 'B' ? 1000 : 0) + (level[1] - '0') * 100 + (level[2] - '0') * 10 + level[3] - '0';
}

static std::set<std::string> parse_selected_levels(const Json::Value &root)
{
    std::set<std::string> parsed;
    if (!root.isArray() || root.empty() || root.size() > 101 || Json::FastWriter().write(root).size() > 4096)
        throw std::runtime_error("invalid selected_levels slot data");
    for (const auto &value : root)
        if (!value.isString() || !access_item_id(value.asString()) || !parsed.insert(value.asString()).second)
            throw std::runtime_error("invalid selected_levels slot data entry");
    return parsed;
}

static std::map<std::string, std::string> parse_placements(int mode, const Json::Value &placements,
                                                           const std::set<std::string> &selected,
                                                           const Json::Value &hub_campaigns)
{
    if (mode < 0 || mode > 2)
        throw std::runtime_error("Invalid level_shuffle slot data");
    if (!placements.isObject() || placements.size() != selected.size() ||
        Json::FastWriter().write(placements).size() > 8192)
        throw std::runtime_error("Invalid level_placements slot data");
    std::set<char> campaigns;
    if (!hub_campaigns.isNull())
    {
        if (!hub_campaigns.isArray() || hub_campaigns.empty() || hub_campaigns.size() > 3)
            throw std::runtime_error("Invalid selected_campaigns slot data");
        for (const auto &game : hub_campaigns)
            if (!game.isUInt() || game.asUInt() < 1 || game.asUInt() > 3 ||
                !campaigns.insert('0' + game.asUInt()).second)
                throw std::runtime_error("Invalid selected_campaigns slot data");
    }
    for (const auto &[destination, source] : secret_stages)
        if (selected.count(destination) && (!selected.count(source) || placements[destination] != destination))
            throw std::runtime_error("Invalid secret-stage placement");
    for (const auto &[destination, source] : c2_secret_entrances)
        if (selected.count(destination) && !selected.count(source))
            throw std::runtime_error("Missing secret-entrance source level");
    std::map<std::string, std::string> result;
    std::set<std::string> destinations;
    for (const auto &source : placements.getMemberNames())
    {
        const auto &destination = placements[source];
        const bool secret = std::any_of(secret_stages.begin(), secret_stages.end(),
                                       [&](const auto &entry) { return source == entry.first; });
        if (!access_item_id(source) || (!selected.count(source) && !ingame::collectible_spec(source.c_str())) ||
            (mode != 2 && !selected.count(source)) ||
            (campaigns.empty() ? !selected.count(source) : (!secret && !campaigns.count(source[1]))) ||
            (secret && destination != source) ||
            !destination.isString() || !selected.count(destination.asString()) ||
            !destinations.insert(destination.asString()).second ||
            ((source[0] == 'B' || !mode) && destination.asString() != source) ||
            (source[0] == 'L' && destination.asString()[0] != 'L') ||
            (mode == 1 && source[1] != destination.asString()[1]))
            throw std::runtime_error("Invalid level destination permutation");
        result.emplace(source, destination.asString());
    }
    return result;
}

bool has_access(const std::vector<std::pair<std::int64_t, int>> &received, const std::string &level)
{
    const auto id = access_item_id(level);
    return id && std::any_of(received.begin(), received.end(), [id](const auto &item) { return item.first == id; });
}

bool level_locked(const SeedConfig &seed, const std::vector<std::pair<std::int64_t, int>> &received,
                  const std::string &level, const std::set<std::int64_t> &cleared)
{
    auto source = level.substr(0, 4);
    if (source.size() == 4 && source[0] == 'S')
    {
        source[0] = 'L';
        const auto route = std::find_if(c2_secret_entrances.begin(), c2_secret_entrances.end(), [&](const auto &entry) {
            return source == entry.first;
        });
        if (route == c2_secret_entrances.end() || !seed.levels.count(source) || !seed.levels.count(route->second) ||
            (seed.level_access_locks && !has_access(received, source)))
            return true;
        if (level_accessible(seed, received, route->second, cleared))
            return false;
        return level_locked(seed, received, "S" + std::string(route->second).substr(1), cleared);
    }
    const auto placement = seed.placements.find(source);
    if (placement == seed.placements.end())
        return true;
    const auto &destination = placement->second;
    auto physical = source;
    for (const auto &[secret, parent] : secret_stages)
        if (source == secret)
            for (const auto &[entrance, target] : seed.placements)
                if (target == parent)
                    physical = entrance;
    const auto bosses = seed.rules.boss_requirements.find(destination);
    return (seed.rules.gated_finals.count(destination) && !cortex_requirements_met(seed.rules, received, cleared)) ||
           (bosses != seed.rules.boss_requirements.end() &&
            !std::all_of(bosses->second.begin(), bosses->second.end(), [&](auto boss) { return cleared.count(boss); })) ||
           !seed.levels.count(destination) ||
           (seed.level_access_locks && !has_access(received, destination)) ||
           !hub_area_unlocked(seed, received, physical[1] - '0', hub_area(physical));
}

bool hub_area_unlocked(const SeedConfig &seed, const std::vector<std::pair<std::int64_t, int>> &received,
                       unsigned campaign, unsigned area)
{
    if (campaign < 1 || campaign > 3 || area < 1 || area > (campaign == 1 ? 3u : 5u))
        return false;
    if (!seed.hub_area_locks || area == 1)
        return true;
    return std::count_if(received.begin(), received.end(), [id = hub_access_item(campaign, area)](const auto &item) {
               return item.first == id;
           }) >= (campaign == 3 ? 1 : area - 1);
}

bool level_accessible(const SeedConfig &seed, const std::vector<std::pair<std::int64_t, int>> &received,
                      const std::string &level, const std::set<std::int64_t> &cleared)
{
    const auto entrance = std::find_if(seed.placements.begin(), seed.placements.end(), [&](const auto &entry) {
        return entry.second == level;
    });
    return entrance != seed.placements.end() && !level_locked(seed, received, entrance->first, cleared);
}

static CampaignGoals parse_goals(const Json::Value &levels, const std::set<std::string> &selected,
                                 const std::map<CheckKey, std::int64_t> &checks, const Json::Value &goal)
{
    if (goal["type"] == "cortex_requirements")
    {
        if (!goal.isObject() || goal.size() != 4 || !goal["boss_mode"].isString() ||
            (goal["boss_mode"] != "cortex" && goal["boss_mode"] != "all_bosses" && goal["boss_mode"] != "cortex_last") ||
            !goal["requirements"].isObject() || goal["requirements"].empty() || goal["requirements"].size() > 2)
            throw std::runtime_error("Invalid Cortex requirements");
        Json::Value base_goal(Json::objectValue);
        base_goal["type"] = goal["boss_mode"];
        base_goal["required"] = goal["required"];
        auto result = parse_goals(levels, selected, checks, base_goal);
        result.type = "cortex_requirements";
        for (const auto &kind : goal["requirements"].getMemberNames())
        {
            const auto &count = goal["requirements"][kind];
            if ((kind != "level_clears" && kind != "token_hunt") || !count.isUInt() || !count.asUInt() ||
                count.asUInt() > (kind == "level_clears" ? result.regular_levels.size() : checks.size()))
                throw std::runtime_error("Invalid Cortex requirement target");
            result.requirements[kind] = count.asUInt();
        }
        return result;
    }
    if (!levels.isArray() || levels.size() > 16 || Json::FastWriter().write(levels).size() > 1024)
        throw std::runtime_error("Invalid or missing goal_levels slot data");
    CampaignGoals result;
    if (!goal.isObject() || goal.size() != 2 || !goal["type"].isString() || !goal["required"].isUInt() ||
        Json::FastWriter().write(goal).size() > 1024)
        throw std::runtime_error("Invalid victory_goal slot data");
    result.type = goal["type"].asString();
    result.required = goal["required"].asUInt();
    if (result.type != "cortex" && result.type != "all_bosses" && result.type != "cortex_last" && result.type != "level_clears" &&
        result.type != "token_hunt")
        throw std::runtime_error("Unknown victory goal type");
    for (const auto &value : levels)
    {
        if (!value.isString())
            throw std::runtime_error("Invalid goal level");
        const auto level = value.asString();
        const bool boss = level.size() == 4 && level[0] == 'B' && level[1] >= '1' && level[1] <= '3' &&
                          level[2] == '0' && level[3] >= '1' && level[3] <= (level[1] == '1' ? '6' : '5');
        if (!boss || (result.type != "all_bosses" && result.type != "cortex_last" && level != "B106" && level != "B205" && level != "B305") ||
            !selected.count(level) || !result.levels.insert(level).second)
            throw std::runtime_error("Goal bosses must be unique selected bosses matching the goal type");
    }
    for (const auto &level : selected)
    {
        if (!access_item_id(level))
            throw std::runtime_error("Invalid selected goal level");
        if (level[0] == 'L')
            result.regular_levels.insert(level);
        const auto check = checks.find({level, "clear"});
        if (check == checks.end() || check->second <= 0)
            throw std::runtime_error("Goal requires a clear check for every selected level");
    }
    if (result.type == "all_bosses" || result.type == "cortex_last")
        for (const auto &level : result.levels)
            for (char number = '1'; number <= (level[1] == '1' ? '6' : '5'); ++number)
                if (!result.levels.count(std::string("B") + level[1] + '0' + number))
                    throw std::runtime_error("All-bosses goal requires every boss in its goal campaigns");
    if (result.type == "cortex" || result.type == "all_bosses" || result.type == "cortex_last")
    {
        if (result.levels.empty() || result.required)
            throw std::runtime_error("Boss goal requires bosses and no counter target");
    }
    else if (!result.levels.empty() || !result.required || result.required > checks.size() ||
             (result.type == "level_clears" && result.required > result.regular_levels.size()))
        throw std::runtime_error("Invalid victory counter target");
    return result;
}

unsigned goal_progress(const CampaignGoals &goals, const std::set<std::string> &completed,
                       const std::map<CheckKey, std::int64_t> &checks, const std::set<std::int64_t> &acknowledged,
                       const std::vector<std::pair<std::int64_t, int>> &received)
{
    if (goals.type == "token_hunt")
        return static_cast<unsigned>(std::count_if(received.begin(), received.end(),
                                                   [](const auto &item) { return item.first == 0xcb0000 + 2203; }));
    const auto &levels = goals.type == "level_clears" ? goals.regular_levels : goals.levels;
    return static_cast<unsigned>(std::count_if(levels.begin(), levels.end(), [&](const auto &level) {
        const auto check = checks.find({level, "clear"});
        return completed.count(level) && check != checks.end() && acknowledged.count(check->second);
    }));
}
bool goals_complete(const CampaignGoals &goals, const std::set<std::string> &completed,
                    const std::map<CheckKey, std::int64_t> &checks, const std::set<std::int64_t> &acknowledged,
                    const std::vector<std::pair<std::int64_t, int>> &received)
{
    if (goals.type == "cortex_requirements")
    {
        CheckRules rules;
        rules.cortex_requirements = goals.requirements;
        for (const auto &level : goals.regular_levels)
        {
            const auto found = checks.find({level, "clear"});
            if (found != checks.end())
                rules.regular_clears.insert(found->second);
        }
        if (!cortex_requirements_met(rules, received, acknowledged))
            return false;
    }
    else if (goals.type != "cortex" && goals.type != "all_bosses" && goals.type != "cortex_last")
        return goal_progress(goals, completed, checks, acknowledged, received) >= goals.required;
    if (goals.levels.empty())
        return false;
    return std::all_of(goals.levels.begin(), goals.levels.end(), [&](const auto &level) {
        const auto check = checks.find({level, "clear"});
        return completed.count(level) && check != checks.end() && acknowledged.count(check->second);
    });
}

static std::map<CheckKey, std::int64_t> parse_checks(const Json::Value &root)
{
    std::map<CheckKey, std::int64_t> parsed;
    if (!root.isObject() || Json::FastWriter().write(root).size() > 1024 * 1024)
        throw std::runtime_error("invalid checks slot data");
    if (root.empty())
        throw std::runtime_error("empty checks slot data");
    for (const auto &id_text : root.getMemberNames())
    {
        const auto &value = root[id_text];
        std::size_t end = 0;
        std::int64_t id = 0;
        try
        {
            id = std::stoll(id_text, &end);
        }
        catch (const std::exception &)
        {
            throw std::runtime_error("invalid checks slot data entry");
        }
        if (end != id_text.size() || id <= 0 || !value.isObject() || !value["level"].isString() ||
            !value["event"].isString() || value["level"].asString().size() != 4 ||
            !parsed.emplace(CheckKey{value["level"].asString(), value["event"].asString()}, id).second)
            throw std::runtime_error("invalid checks slot data entry");
        const auto event = value["event"].asString(), level = value["level"].asString();
        if (event.rfind("box:", 0) == 0 &&
            std::none_of(std::begin(box_specs), std::end(box_specs), [&](const auto &box) {
                return id == box.id && level == box.level && event == box.event_name();
            }))
            throw std::runtime_error("Box check differs from pinned crate manifest");
    }
    return parsed;
}

SeedConfig::SeedConfig(const Json::Value &data)
{
    if (data["protocol"] != seed_protocol)
        throw std::runtime_error("Unsupported seed protocol. Generate a seed with the current APWorld");
    if (!data["level_shuffle"].isInt())
        throw std::runtime_error("Invalid seed shuffle mode");
    if (!data["hub_area_locks"].isBool() || !data["level_access_locks"].isBool())
        throw std::runtime_error("Invalid seed lock settings");
    hub_area_locks = data["hub_area_locks"].asBool();
    level_access_locks = data["level_access_locks"].asBool();
    checks = parse_checks(data["checks"]);
    levels = parse_selected_levels(data["selected_levels"]);
    shuffle = data["level_shuffle"].asInt();
    placements = parse_placements(shuffle, data["level_placements"], levels, data["selected_campaigns"]);
    goals = parse_goals(data["goal_levels"], levels, checks, data["victory_goal"]);
    rules = compile_check_rules(checks, data, levels);
    const auto &styles = data["movement_styles"];
    if (styles.isObject())
        for (const auto &level : styles.getMemberNames())
            if (levels.count(level) && styles[level].isUInt() && styles[level].asUInt() >= 1 &&
                styles[level].asUInt() <= 3)
                movement_styles[level] = styles[level].asUInt();
}
