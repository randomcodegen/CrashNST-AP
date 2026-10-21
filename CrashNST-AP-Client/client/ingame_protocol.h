#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <string>
#include <type_traits>
#include "collectibles.h"

namespace ingame
{
    enum MarkerMode : unsigned
    {
        MarkersOff,
        MarkersBounds,
        MarkersTint
    };
    inline bool marker_bounds(unsigned mode) { return mode == MarkersBounds; }
    inline unsigned item_color(unsigned flags) { return (flags & 1) ? 0xffdda0dd : (flags & 2) ? 0xffcd5a6a
                                                                               : (flags & 4)   ? 0xff7280fa
                                                                                               : 0xffffff00; }
    constexpr unsigned box_colors[]{0, 0xffdda0dd, 0xffcd5a6a, 0xff7280fa, 0xffffff00};
    inline unsigned box_color_index(unsigned color)
    {
        for (unsigned i = 0; i < 5; ++i)
            if (box_colors[i] == color)
                return i;
        return 5;
    }
    constexpr unsigned protocol = 19, max_boxes = 8192, max_levels = 112;
    constexpr unsigned default_hub_distance = 250, max_hub_distance = 100000;
    constexpr std::array<unsigned, 3> default_hub_distances{default_hub_distance, default_hub_distance, default_hub_distance};
    constexpr float default_tint_opacity = 0.75f;
    inline bool valid_tint_opacity(float opacity) { return std::isfinite(opacity) && opacity >= 0 && opacity <= 1; }
    inline std::array<float, 4> tint_blend(unsigned color, float opacity)
    {
        // HDR color compensates for crate textures and lighting to approximate pale cyan (#8FD8D8)
        if (!color)
            return {0.8f, 3.25f, 10.5f, opacity};
        return {(color & 255) * 0.8f / 143, ((color >> 8) & 255) * 3.25f / 216, ((color >> 16) & 255) * 10.5f / 216, opacity};
    }
    constexpr unsigned startup_handover = 2;
    constexpr unsigned max_lines = 4096, max_popup_lines = 64, max_spans = 16, max_commands = 32;
    struct Span
    {
        std::uint32_t color = 0xffeeeeee;
        char text[256]{};
    };
    struct Line
    {
        unsigned count = 0;
        std::uint64_t ticks = 0;
        Span spans[max_spans]{};
    };
    struct Box
    {
        char level[5]{}, name[128]{};
        float position[3]{};
        unsigned color = 0;
    };
    struct Level
    {
        char key[5]{}, title[160]{}, counts[48]{}, collectible_counts[128]{};
        unsigned style = 0;
    };
    struct Entrance
    {
        char hub[5]{}, destination[5]{}, source[5]{};
        float position[3]{};
        int c3_button = -1;
    };
    struct Collectible
    {
        char key[5]{};
        unsigned owned = 0, selected = 0, blocked = 0, checked = 0;
    };
    struct CollectibleObservation
    {
        char key[5]{};
        unsigned mask = 0;
    };

    struct Shared
    {
        unsigned version = protocol, bytes = sizeof(Shared), client_pid = 0;
        std::uint64_t heartbeat = 0, revision = 0;
        std::uint64_t line_write = 0, line_clear = 0;
        unsigned line_begin = 0;
        unsigned stop = 0, attached = 0, running = 0, console_open = 0, visible = 1, vanilla_controls = 0;
        unsigned startup_popup = 0, marker_mode = MarkersTint;
        unsigned death_link = 0, death_request = 0, death_ack = 0, deaths = 0, applied_style = 0;
        unsigned box_count = 0, level_count = 0, entrance_count = 0, line_count = 0;
        unsigned collectible_count = 0;
        unsigned collectible_awards = 0;
        unsigned collectible_observation_count = 0;
        unsigned c3_buttons_ready = 0;
        std::uint64_t c3_buttons_active = 0;
        float box_half_size = 50;
        std::array<unsigned, 3> hub_label_distances = default_hub_distances;
        float tint_opacity = default_tint_opacity;
        unsigned command_read = 0, command_write = 0;
        char status[256]{}, binding[256]{}, error[256]{};
        char campaign_counts[3][48]{};
        char goal_counts[64]{};
        char completion[1024 * 1024]{}; // NUL-separated commands, item names, location names.
        unsigned completion_bytes = 0, item_start = 0, location_start = 0;
        Box boxes[max_boxes]{};
        Level levels[max_levels]{};
        Entrance entrances[max_levels]{};
        Collectible collectibles[max_levels]{};
        CollectibleObservation collectible_observations[max_levels]{};
        Line lines[max_lines]{};
        char commands[max_commands][1024]{};
    };
    static_assert(std::is_standard_layout_v<Shared> && std::is_trivially_copyable_v<Shared>);
    inline const Line &console_line(const Shared &s, unsigned index) { return s.lines[(s.line_begin + index) % max_lines]; }
    inline void append_console_line(Shared &s, const Line &line)
    {
        s.lines[(s.line_begin + s.line_count) % max_lines] = line;
        if (s.line_count == max_lines)
            s.line_begin = (s.line_begin + 1) % max_lines;
        else
            ++s.line_count;
        ++s.line_write;
        ++s.revision;
    }
    inline void clear_console_lines(Shared &s)
    {
        s.line_count = 0;
        s.line_begin = 0;
        ++s.line_clear;
        ++s.revision;
    }
    inline void copy_console_snapshot(Shared &to, const Shared &from)
    {
        // unchanged frames copy no lines, and appends copy only new slots
        const auto added = from.line_clear != to.line_clear || from.line_write < to.line_write ? from.line_count : static_cast<unsigned>(std::min<std::uint64_t>(from.line_write - to.line_write, from.line_count));
        for (unsigned i = from.line_count - added; i < from.line_count; ++i)
        {
            const auto index = (from.line_begin + i) % max_lines;
            to.lines[index] = from.lines[index];
        }
        const bool published = to.revision != from.revision;
        auto *destination = reinterpret_cast<unsigned char *>(&to);
        const auto *source = reinterpret_cast<const unsigned char *>(&from);
        std::memcpy(destination, source, offsetof(Shared, completion));
        if (published)
            std::memcpy(destination + offsetof(Shared, completion), source + offsetof(Shared, completion),
                        offsetof(Shared, levels) - offsetof(Shared, completion));
        std::memcpy(destination + offsetof(Shared, levels), source + offsetof(Shared, levels),
                    offsetof(Shared, lines) - offsetof(Shared, levels));
        std::memcpy(to.commands, from.commands, sizeof(from.commands));
    }
    template <std::size_t N>
    void copy(char (&to)[N], const std::string &from)
    {
        const auto size = (from.size() < N - 1) ? from.size() : N - 1;
        std::memcpy(to, from.data(), size);
        to[size] = 0;
    }
    inline std::wstring name(DWORD pid, const wchar_t *suffix)
    {
        return L"Local\\CrashNST_AP_" + std::to_wstring(pid) + suffix;
    }
    inline bool valid(const Shared &s)
    {
        if (!(s.version == protocol && s.bytes == sizeof(Shared) && s.box_count <= max_boxes &&
              s.marker_mode <= MarkersTint && s.level_count <= max_levels && s.entrance_count <= max_levels && s.line_count <= max_lines && s.line_begin < max_lines && s.line_write >= s.line_count && s.collectible_count <= max_levels && s.collectible_observation_count <= max_levels && s.c3_buttons_ready <= 1 && !(s.c3_buttons_active >> 36) &&
              s.completion_bytes <= sizeof(s.completion) && s.item_start <= s.location_start &&
              s.location_start <= s.completion_bytes && s.command_write - s.command_read <= max_commands &&
              std::isfinite(s.box_half_size) && s.box_half_size >= 10 && s.box_half_size <= 250 &&
              std::all_of(s.hub_label_distances.begin(), s.hub_label_distances.end(), [](unsigned distance) { return distance <= max_hub_distance; }) && valid_tint_opacity(s.tint_opacity)))
            return false;
        const auto terminated = [](const auto &text)
        { return std::memchr(text, 0, sizeof(text)) != nullptr; };
        if (!terminated(s.status) || !terminated(s.binding) || !terminated(s.error) || !terminated(s.goal_counts))
            return false;
        for (const auto &counts : s.campaign_counts)
            if (!terminated(counts))
                return false;
        for (unsigned i = 0; i < s.collectible_count; ++i)
        {
            const auto &c = s.collectibles[i];
            if (!terminated(c.key))
                return false;
            const auto spec = collectible_spec(c.key);
            if (!spec || ((c.owned | c.selected | c.blocked | c.checked) & ~collectible_bits(*spec)) ||
                ((c.blocked | c.checked) & ~c.selected))
                return false;
            for (unsigned j = 0; j < i; ++j)
                if (!std::strcmp(c.key, s.collectibles[j].key))
                    return false;
        }
        for (unsigned i = 0; i < s.collectible_observation_count; ++i)
        {
            const auto &c = s.collectible_observations[i];
            if (!terminated(c.key))
                return false;
            const auto spec = collectible_spec(c.key);
            if (!spec || (c.mask & ~collectible_mask(*spec)))
                return false;
            for (unsigned j = 0; j < i; ++j)
                if (!std::strcmp(c.key, s.collectible_observations[j].key))
                    return false;
        }
        for (unsigned i = 0; i < s.box_count; ++i)
            if (!terminated(s.boxes[i].level) || !terminated(s.boxes[i].name) || box_color_index(s.boxes[i].color) >= 5)
                return false;
        for (unsigned i = 0; i < s.level_count; ++i)
            if (!terminated(s.levels[i].key) || !terminated(s.levels[i].title) || !terminated(s.levels[i].counts) || !terminated(s.levels[i].collectible_counts) || s.levels[i].style > 3)
                return false;
        for (unsigned i = 0; i < s.entrance_count; ++i)
            if (!terminated(s.entrances[i].hub) || !terminated(s.entrances[i].destination) || !terminated(s.entrances[i].source) ||
                s.entrances[i].c3_button < -1 || s.entrances[i].c3_button >= 36)
                return false;
        for (unsigned i = 0; i < s.line_count; ++i)
        {
            const auto &line = console_line(s, i);
            if (line.count > max_spans)
                return false;
            for (unsigned j = 0; j < line.count; ++j)
                if (!terminated(line.spans[j].text))
                    return false;
        }
        return true;
    }
}
