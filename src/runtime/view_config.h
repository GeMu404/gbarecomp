#pragma once

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>

namespace gbarecomp {

// Native GBA picture. Everything wide/tall is expressed relative to this.
constexpr std::uint32_t kNativeViewWidth = 240;
constexpr std::uint32_t kNativeViewHeight = 160;

// Single source of truth for the extended view: the game declares only the
// height ([video] view_height, e.g. 224 = SNES) and optionally an aspect
// ([video] aspect = "16:9"). Width = round(height * aw / ah); the extra rows
// are split evenly above and below the native 160.
struct DerivedView {
    std::uint32_t width = kNativeViewWidth;
    std::uint32_t extra_top = 0;
    std::uint32_t extra_bottom = 0;
};

inline DerivedView derive_view_from_height(std::uint32_t height,
                                           std::uint32_t aspect_w = 16,
                                           std::uint32_t aspect_h = 9) {
    DerivedView v;
    if (height <= kNativeViewHeight || aspect_w == 0 || aspect_h == 0) return v;
    const std::uint32_t w = (height * aspect_w + aspect_h / 2u) / aspect_h;
    v.width = std::max(kNativeViewWidth, w);
    const std::uint32_t extra = height - kNativeViewHeight;
    v.extra_top = extra / 2u;
    v.extra_bottom = extra - extra / 2u;
    return v;
}

// Minimal pre-scan of [video] view_height / aspect so RunOptions can be
// derived before the launcher runs. Returns 0 when not declared.
inline std::uint32_t read_config_view_height(const std::string& path,
                                             std::uint32_t* aspect_w,
                                             std::uint32_t* aspect_h) {
    std::ifstream in(path);
    if (!in) return 0;
    std::string line, section;
    std::uint32_t height = 0;
    auto trim = [](std::string t) {
        const auto a = t.find_first_not_of(" \t\r\"");
        const auto b = t.find_last_not_of(" \t\r\"");
        return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    };
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') { section = line; continue; }
        if (section != "[video]") continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (key == "view_height") {
            height = static_cast<std::uint32_t>(std::stoul("0" + val));
        } else if (key == "aspect" && aspect_w && aspect_h) {
            const auto c = val.find(':');
            if (c != std::string::npos) {
                *aspect_w = static_cast<std::uint32_t>(std::stoul("0" + val.substr(0, c)));
                *aspect_h = static_cast<std::uint32_t>(std::stoul("0" + val.substr(c + 1)));
            }
        }
    }
    return height;
}

struct ViewGeometry {
    std::uint32_t width = 240;
    std::uint32_t extra_left = 0;
    std::uint32_t extra_right = 0;
};

struct ViewVerticalMargins {
    std::uint32_t extra_top = 0;
    std::uint32_t extra_bottom = 0;
};

// Vertical margins for an already-resolved horizontal view. Matches
// RunOptions: tall_bottom_anchor → 64/0; else explicit view_extra_*;
// else legacy 32/32 letterbox. Native (non-expanded) width → 0/0.
inline ViewVerticalMargins resolve_view_vertical_margins(
    bool horizontally_expanded,
    bool tall_bottom_anchor,
    std::uint16_t view_extra_top,
    std::uint16_t view_extra_bottom) {
    if (!horizontally_expanded) {
        return {};
    }
    if (view_extra_top != 0 || view_extra_bottom != 0) {
        return {view_extra_top, view_extra_bottom};
    }
    if (tall_bottom_anchor) {
        return {64u, 0u};
    }
    return {32u, 32u};
}

inline bool legacy_extra_to_view_width(int extra_per_side, int* width) {
    if (!width || extra_per_side < 0 ||
        extra_per_side > (std::numeric_limits<int>::max() - 240) / 2) {
        return false;
    }
    *width = 240 + 2 * extra_per_side;
    return true;
}

// Pure policy resolver kept separate from the renderer so capability and
// faithful-default behavior can be unit-tested without launching a game.
inline ViewGeometry resolve_view_geometry(int requested_width,
                                          std::uint32_t game_max_width,
                                          bool development_override,
                                          std::uint32_t engine_max_width) {
    constexpr std::uint32_t kNativeWidth = 240;
    const std::uint32_t engine_max = std::max(kNativeWidth, engine_max_width);
    const std::uint32_t opted_in_max =
        std::clamp(game_max_width, kNativeWidth, engine_max);
    const std::uint32_t allowed_max =
        development_override ? engine_max : opted_in_max;
    const std::uint32_t requested = requested_width < 240
        ? kNativeWidth
        : static_cast<std::uint32_t>(requested_width);
    const std::uint32_t width = std::min(requested, allowed_max);
    const std::uint32_t extra = width - kNativeWidth;
    return {width, extra / 2u, extra - extra / 2u};
}

// Convert a host drawable aspect ratio into a logical GBA view width while
// keeping the authentic 160-line height. Narrower-than-native windows never
// crop the game; wider windows reveal more horizontal content up to game_max.
inline std::uint32_t resize_driven_view_width(int drawable_width,
                                              int drawable_height,
                                              std::uint32_t game_max_width,
                                              std::uint32_t engine_max_width) {
    constexpr std::uint32_t kNativeWidth = 240;
    constexpr std::uint32_t kNativeHeight = 160;
    const std::uint32_t maximum = std::clamp(
        game_max_width, kNativeWidth,
        std::max(kNativeWidth, engine_max_width));
    if (drawable_width <= 0 || drawable_height <= 0) return kNativeWidth;

    // Round to the nearest logical pixel. Use 64-bit arithmetic so very large
    // desktop dimensions cannot overflow before the clamp.
    const std::uint64_t scaled =
        static_cast<std::uint64_t>(drawable_width) * kNativeHeight;
    const std::uint64_t rounded =
        (scaled + static_cast<std::uint64_t>(drawable_height) / 2u) /
        static_cast<std::uint64_t>(drawable_height);
    return static_cast<std::uint32_t>(
        std::clamp<std::uint64_t>(rounded, kNativeWidth, maximum));
}

}  // namespace gbarecomp
