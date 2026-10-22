#include "console.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <memory>
#include <set>

namespace ingame
{
    namespace
    {
        std::size_t previous_character(const std::string &text, std::size_t position)
        {
            if (position)
                --position;
            while (position && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80)
                --position;
            return position;
        }
        std::size_t next_character(const std::string &text, std::size_t position)
        {
            if (position < text.size())
                ++position;
            while (position < text.size() && (static_cast<unsigned char>(text[position]) & 0xc0) == 0x80)
                ++position;
            return position;
        }
    }
    void ConsoleInput::set(std::string value)
    {
        text.clear();
        cursor = anchor = 0;
        insert(std::move(value));
    }
    void ConsoleInput::move(int direction, bool selecting)
    {
        if (!selecting && cursor != anchor)
            cursor = direction < 0 ? std::min(cursor, anchor) : std::max(cursor, anchor);
        else
            cursor = direction < 0 ? previous_character(text, cursor) : next_character(text, cursor);
        if (!selecting)
            anchor = cursor;
    }
    void ConsoleInput::edge(bool end, bool selecting)
    {
        cursor = end ? text.size() : 0;
        if (!selecting)
            anchor = cursor;
    }
    std::string ConsoleInput::selected() const { return text.substr(std::min(cursor, anchor), std::max(cursor, anchor) - std::min(cursor, anchor)); }
    void ConsoleInput::insert(std::string value)
    {
        // strip clipboard newlines
        for (auto &c : value)
            if (c == '\r' || c == '\n' || c == '\t')
                c = ' ';
        value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c)
                                   { return c < 32 || c == 127; }),
                    value.end());
        if (value.empty())
            return;
        const auto first = std::min(cursor, anchor), last = std::max(cursor, anchor);
        auto size = std::min(value.size(), 1000 - (text.size() - (last - first)));
        while (size < value.size() && size && (static_cast<unsigned char>(value[size]) & 0xc0) == 0x80)
            --size;
        text.replace(first, last - first, value, 0, size);
        cursor = anchor = first + size;
    }
    void ConsoleInput::erase(bool backwards)
    {
        auto first = std::min(cursor, anchor), last = std::max(cursor, anchor);
        if (first == last)
        {
            if (backwards)
                first = previous_character(text, first);
            else
                last = next_character(text, last);
        }
        text.erase(first, last - first);
        cursor = anchor = first;
    }
    const std::vector<std::string> &commands()
    {
        static const std::vector<std::string> names{
            "/help", "/server", "/port", "/slot", "/password", "/connect", "/disconnect", "/status", "/quit",
            "/crashsanity", "/markers", "/boxmarkers", "/outoflogic", "/hubdistance", "/hubdistance_c1", "/hubdistance_c2", "/hubdistance_c3", "/tint_opacity", "/clear", "!help", "!license", "!options", "!admin", "!players", "!getitem", "!hint", "!hint_location", "!remaining",
            "!missing", "!checked", "!status", "!release", "!collect", "!countdown", "!alias"};
        return names;
    }
    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                       { return std::tolower(c); });
        return s;
    }
    unsigned box_marker_mode(const std::string &name)
    {
        const auto text = lower(name);
        if (text == "off")
            return MarkersOff;
        if (text == "bounds")
            return MarkersBounds;
        if (text == "tint")
            return MarkersTint;
        throw std::invalid_argument("Use /boxmarkers bounds, tint or off");
    }
    const char *box_marker_mode_name(unsigned mode)
    {
        return mode == MarkersOff ? "off" : mode == MarkersBounds ? "bounds"
                                                                  : "tint";
    }
    std::vector<std::string> complete(const std::string &input, const std::vector<std::string> &cmds,
                                      const std::vector<std::string> &items, const std::vector<std::string> &locations)
    {
        static const std::vector<std::string> marker_modes{"bounds", "tint", "off"};
        static const std::vector<std::string> switches{"on", "off"};
        std::string prefix;
        const auto *names = &cmds;
        if (input.rfind("!hint_location ", 0) == 0)
        {
            prefix = "!hint_location ";
            names = &locations;
        }
        else if (input.rfind("!hint ", 0) == 0)
        {
            prefix = "!hint ";
            names = &items;
        }
        else if (input.rfind("/boxmarkers ", 0) == 0)
        {
            prefix = "/boxmarkers ";
            names = &marker_modes;
        }
        else if (input.rfind("/outoflogic ", 0) == 0)
        {
            prefix = "/outoflogic ";
            names = &switches;
        }
        else if (input.find(' ') != std::string::npos)
            return {};
        const auto needle = lower(input.substr(prefix.size()));
        std::vector<std::string> matches;
        for (const auto &name : *names)
            if (lower(name).rfind(needle, 0) == 0)
                matches.push_back(prefix + name);
        std::sort(matches.begin(), matches.end());
        matches.erase(std::unique(matches.begin(), matches.end()), matches.end());
        return matches;
    }
    std::vector<std::string> unpack_names(const Shared &state, unsigned begin, unsigned end)
    {
        std::vector<std::string> result;
        if (!valid(state) || begin > end || end > state.completion_bytes)
            return result;
        while (begin < end)
        {
            const auto stop = static_cast<const char *>(std::memchr(state.completion + begin, 0, end - begin));
            if (!stop)
                return {};
            result.emplace_back(state.completion + begin, stop);
            begin = static_cast<unsigned>(stop - state.completion) + 1;
        }
        return result;
    }
    bool valid_server(const std::string &server)
    {
        auto host = server;
        if (host.rfind("wss://", 0) == 0)
            host.erase(0, 6);
        else if (host.rfind("ws://", 0) == 0)
            host.erase(0, 5);
        if (host.empty() || host.size() > 240 || host.find_first_of("/\\ \t\r\n") != std::string::npos)
            return false;
        if (std::any_of(host.begin(), host.end(), [](unsigned char c)
                        { return c < 32 || c == 127; }))
            return false;
        const auto colon = host.rfind(':');
        if (colon == std::string::npos)
            return host.find('[') == std::string::npos && host.find(']') == std::string::npos;
        if (!colon || colon + 1 == host.size())
            return false;
        const auto address = host.substr(0, colon), port = host.substr(colon + 1);
        if (address.find(':') != std::string::npos && (address.front() != '[' || address.back() != ']'))
            return false;
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c)
                                            { return std::isdigit(c); }))
            return false;
        const auto value = std::stoul(port);
        return value > 0 && value <= 65535;
    }
    bool main_menu(std::string zone)
    {
        zone = lower(zone);
        if (zone.rfind("maps/", 0) == 0)
            zone.erase(0, 5);
        return zone == "crash1/c1_startscreen/c1_startscreen";
    }
    float tint_opacity(const std::string &value)
    {
        std::size_t end = 0;
        float opacity;
        try
        {
            opacity = std::stof(value, &end);
        }
        catch (const std::exception &)
        {
            throw std::invalid_argument("Use /tint_opacity 0..1");
        }
        if (end != value.size() || !valid_tint_opacity(opacity))
            throw std::invalid_argument("Use /tint_opacity 0..1");
        return opacity;
    }
    unsigned hub_distance(const std::string &value)
    {
        if (value.empty() || value.size() > 6 ||
            !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
            throw std::invalid_argument("Use /hubdistance 0..100000 (0 hides labels)");
        const auto distance = std::stoul(value);
        if (distance > max_hub_distance)
            throw std::invalid_argument("Use /hubdistance 0..100000 (0 hides labels)");
        return static_cast<unsigned>(distance);
    }
    bool entrance_label_visible(const std::array<float, 3> &player, const float *entrance, unsigned distance)
    {
        const float dx = player[0] - entrance[0], dy = player[1] - entrance[1], dz = player[2] - entrance[2];
        return distance && dx * dx + dy * dy + dz * dz <= static_cast<float>(distance) * distance;
    }
    std::vector<std::string> entrance_list(const Shared &state, unsigned campaign)
    {
        std::vector<std::string> result;
        std::set<std::string> seen;
        const auto title = [&](const char *key) {
            for (unsigned i = 0; i < state.level_count; ++i)
                if (!std::strcmp(state.levels[i].key, key))
                    return std::string(state.levels[i].title);
            return std::string(key);
        };
        for (unsigned i = 0; i < state.entrance_count; ++i)
        {
            const auto &entry = state.entrances[i];
            if (campaign < 1 || campaign > 3 || entry.hub[1] != '0' + campaign || !entry.source[0] || !seen.insert(entry.source).second)
                continue;
            const bool secret = entry.source[0] == 'S';
            auto row = secret ? title(entry.destination) + " (Secret entrance)" : title(entry.source);
            if (!secret && std::strcmp(entry.source, entry.destination))
                row += " -> " + title(entry.destination);
            for (unsigned level = 0; level < state.level_count; ++level)
                if (!std::strcmp(state.levels[level].key, entry.destination))
                {
                    row += "\nChecks: " + std::string(state.levels[level].counts);
                    if (state.levels[level].collectible_counts[0])
                        row += '\n' + std::string(state.levels[level].collectible_counts);
                    break;
                }
            result.push_back(std::move(row));
        }
        return result;
    }
    std::array<POINT, 3> campaign_label_positions(unsigned width, unsigned height)
    {
        // HUD panel centers horizontally
        const float scale = std::min(width / 1920.0f, height / 1080.0f);
        const float left = (width - 1920 * scale) / 2;
        std::array<POINT, 3> result{};
        const float x[]{398, 965, 1510};
        for (unsigned i = 0; i < 3; ++i)
            result[i] = {static_cast<LONG>(left + x[i] * scale), static_cast<LONG>(height > 30 ? height - 30 : 0)};
        return result;
    }
    void PopupQueue::update(std::uint64_t now)
    {
        while (!visible.empty() && now - visible.front().started >= 3500)
            visible.pop_front();
        while (visible.size() < 3 && !pending.empty())
        {
            visible.push_back({std::move(pending.front()), now});
            pending.pop_front();
        }
    }
    float popup_opacity(std::uint64_t now, std::uint64_t started)
    {
        const auto elapsed = now - started;
        return elapsed <= 3000 ? 1.0f : elapsed >= 3500 ? 0.0f
                                                        : static_cast<float>(3500 - elapsed) / 500;
    }
    unsigned faded_color(unsigned color, float opacity)
    {
        return (color & 0xffffff) | (static_cast<unsigned>((color >> 24) * std::clamp(opacity, 0.0f, 1.0f) + 0.5f) << 24);
    }
}
