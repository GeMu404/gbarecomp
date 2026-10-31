#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace gbarecomp {

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
