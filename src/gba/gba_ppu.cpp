// gba_ppu.cpp — see gba_ppu.h.

#include "gba_ppu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "snapshot.h"

extern "C" int smw_ws_mode0_bg_sample(unsigned tile_x, unsigned tile_y,
                                     std::uint16_t* out);

namespace gba {

// Widescreen margin tilemap provider (Step C). When set (by the runtime-side
// sidecar) and the wide path samples a margin column (hardware x outside
// 0..239), the BG text-tilemap entry is sourced from this hook (the true
// extended world) instead of the wrapped 256px ring. Returns nonzero + fills
// *out_entry when data is available. nullptr (default) = vanilla wide behavior.
// Layering-clean: the PPU only calls a nullable pointer; the sidecar owns it.
extern "C" int (*g_ws_tilemap_provider)(int bg, int hw_x, int screen_y,
                                        uint16_t* out_entry) = nullptr;
extern "C" int (*g_ws_bg_x_provider)(int bg, int output_x, int screen_y,
                                     int* out_hw_x) = nullptr;
extern "C" unsigned g_ws_bg_x_provider_layers = 0xFu;
extern "C" int g_ws_affine_filter_enabled = 0;
extern "C" int (*g_ws_affine_filter_provider)(int, int) = nullptr;
extern "C" int g_ws_authored_margin_layers = 0;

// Widescreen pillarbox (Step C policy): when nonzero, the wide path renders the
// margin columns (outside the central 240) as solid black instead of extended
// world — used for non-overworld screens (menus, battles, transitions) so they
// are letterboxed rather than garbled. Set per-frame by the runtime policy.
extern "C" int g_ws_pillarbox = 0;
extern "C" int g_ws_pillarbox_left = 0;
extern "C" int g_ws_pillarbox_right = 0;
extern "C" int g_ws_scroll_bias_x = 0;
extern "C" int g_ws_scroll_bias_y = 0;
extern "C" int g_ws_cam_correction = 0;
extern "C" int g_ws_cam_guest_l1 = 0;
extern "C" int g_ws_view_origin_x = 0;
extern "C" int g_ws_room_w = 0;
extern "C" int g_ws_block_top_peek = 0;
extern "C" int g_ws_player_screen_x = 0;
extern "C" int g_ws_player_screen_y = 0;
extern "C" unsigned g_ws_extra_top = 0;
extern "C" unsigned g_ws_extra_bottom = 0;
extern "C" int g_ws_status_tile_scrub = 0;
extern "C" int g_ws_status_ui_pin = 0;
extern "C" int g_ws_host_obj_compose = 0;  // 1 = world OBJ from host post-latch
extern "C" int (*g_ws_obj_x_provider)(int, int*) = nullptr;
extern "C" int (*g_ws_obj_attr_x_provider)(int, std::uint16_t,
                                            std::uint16_t, std::uint16_t,
                                            int*) = nullptr;
extern "C" int (*g_ws_obj_y_provider)(int, std::uint16_t, int*) = nullptr;
extern "C" void smw_ws_expire_obj_x(void);
extern "C" void (*g_ws_post_latch_hook)(std::uint8_t*, std::uint32_t,
                                         std::uint32_t, std::uint32_t,
                                         std::uint32_t) = nullptr;

GbaPpu::GbaPpu()  = default;
GbaPpu::~GbaPpu() = default;

void GbaPpu::serialize(gbarecomp::debug::SnapshotWriter& w) const {
    w.u32(scanline_);
    w.u32(dot_in_scanline_);
    w.u32(cycle_in_dot_);
    w.u16(vcount_);
    w.u64(frame_count_);
    w.boolean(has_latched_fb_);
    // Snapshot layout remains the fixed native 240x160 payload. At native width
    // retain the historical byte-for-byte path. A wide latch is row-major at
    // render_width(), so its first kFramebufferBytes are NOT the native image;
    // explicitly crop the authentic center instead. This also makes a wide save
    // useful when loaded later in faithful mode.
    if (!view_expanded()) {
        w.bytes(latched_fb_.data(), kFramebufferBytes);
    } else {
        std::array<uint8_t, kFramebufferBytes> center{};
        constexpr std::size_t kNativeStride = kScreenWidth * 3u;
        const std::size_t wide_stride = render_width() * 3u;
        const std::size_t center_x = extra_left_ * 3u;
        const std::size_t center_y0 = extra_top_;
        for (std::size_t y = 0; y < kScreenHeight; ++y) {
            std::memcpy(center.data() + y * kNativeStride,
                        latched_fb_.data() +
                            (y + center_y0) * wide_stride + center_x,
                        kNativeStride);
        }
        w.bytes(center.data(), center.size());
    }
}

void GbaPpu::deserialize(gbarecomp::debug::SnapshotReader& r) {
    scanline_        = r.u32();
    dot_in_scanline_ = r.u32();
    cycle_in_dot_    = r.u32();
    vcount_          = r.u16();
    frame_count_     = r.u64();
    const bool serialized_latch = r.boolean();
    if (!view_expanded()) {
        // Historical native-width restore path, deliberately unchanged.
        has_latched_fb_ = serialized_latch;
        r.bytes(latched_fb_.data(), kFramebufferBytes);
        // Seed the scanline compositor with the restored frame: a state saved
        // mid-frame resumes rendering at scanline_, so rows at and below it
        // are not re-rendered until the frame after next. Without the seed
        // those rows of the first latched frame would be stale garbage.
        std::memcpy(work_fb_.data(), latched_fb_.data(), kFramebufferBytes);
    } else {
        // The fixed-size payload has no authored wide margins. Consume it to
        // preserve snapshot framing, then invalidate only the presentation
        // latch; the next present renders a fresh wide frame from restored GBA
        // state. Device timing/frame counters above remain fully restored.
        std::array<uint8_t, kFramebufferBytes> native_latch{};
        r.bytes(native_latch.data(), native_latch.size());
        has_latched_fb_ = false;
        // Seed the compositor's authentic center from the restored native
        // frame (margins stay whatever the pillarbox policy paints), so rows
        // the resumed mid-frame render never revisits before the next latch
        // show the restored image instead of stale pre-load pixels.
        constexpr std::size_t kNativeStride = kScreenWidth * 3u;
        const std::size_t wide_stride = render_width() * 3u;
        const std::size_t center_x = extra_left_ * 3u;
        const std::size_t center_y0 = extra_top_;
        for (std::size_t y = 0; y < kScreenHeight; ++y) {
            std::memcpy(work_fb_.data() +
                            (y + center_y0) * wide_stride + center_x,
                        native_latch.data() + y * kNativeStride,
                        kNativeStride);
        }
        // A host may re-present immediately after load, before a game-owned
        // state-epoch hook has invalidated/rebuilt its margin caches. Fail those
        // unauthored margins closed for that interim render; the game's normal
        // publish policy reauthorizes them once restored-state data is ready.
        g_ws_pillarbox = g_ws_authored_margin_layers ? 0 : 1;
        g_ws_pillarbox_left = 0;
        g_ws_pillarbox_right = 0;
    }
    // The snapshot format predates the PPU's internal affine reference
    // accumulator. Reconstruct it from live BGxX/Y on the next scanline.
    affine_line_ = {};
}

void GbaPpu::reset() {
    scanline_ = 0;
    dot_in_scanline_ = 0;
    cycle_in_dot_ = 0;
    vcount_ = 0;
    frame_count_ = 0;
    has_latched_fb_ = false;
    affine_line_ = {};
    std::memset(latched_fb_.data(), 0xFF, latched_fb_.size());
    std::memset(work_fb_.data(), 0xFF, work_fb_.size());
}

void GbaPpu::set_view_margins(uint32_t left, uint32_t right,
                              uint32_t top, uint32_t bottom) {
    // Clamp each side to its compile-time max.
    const uint32_t next_left = left > kMaxExtraX ? kMaxExtraX : left;
    const uint32_t next_right = right > kMaxExtraX ? kMaxExtraX : right;
    const uint32_t next_top = top > kMaxExtraY ? kMaxExtraY : top;
    const uint32_t next_bottom = bottom > kMaxExtraY ? kMaxExtraY : bottom;
    if (next_left != extra_left_ || next_right != extra_right_ ||
        next_top != extra_top_ || next_bottom != extra_bottom_) {
        // A latched frame is laid out at the old stride. Force one fresh
        // render after a live resize rather than interpreting it at the new
        // dimensions. Timing state and guest memory remain untouched.
        has_latched_fb_ = false;
    }
    extra_left_ = next_left;
    extra_right_ = next_right;
    extra_top_ = next_top;
    extra_bottom_ = next_bottom;
    g_ws_extra_top = extra_top_;
    g_ws_extra_bottom = extra_bottom_;
}

GbaPpu::TickEvents GbaPpu::tick(uint32_t cycles, uint16_t vcount_compare) {
    // Coarse but correct: walk dot-by-dot, advancing scanline counters
    // as we cross dot boundaries. The cycle_in_dot_ accumulator lets
    // sub-dot ticks (less than 4 cycles) stash residue for the next
    // call.
    TickEvents ev{};
    cycle_in_dot_ += cycles;
    while (cycle_in_dot_ >= kCyclesPerDot) {
        cycle_in_dot_ -= kCyclesPerDot;
        bool was_visible = (dot_in_scanline_ < kDotsVisible);
        ++dot_in_scanline_;
        if (was_visible && dot_in_scanline_ == kDotsVisible) {
            ev.hblank_started = true;
        }
        if (dot_in_scanline_ >= kDotsPerScanline) {
            dot_in_scanline_ = 0;
            bool was_in_visible_window = (scanline_ < kLinesVisible);
            ++scanline_;
            if (scanline_ >= kLinesTotal) {
                scanline_ = 0;
                ++frame_count_;
                ev.frame_completed = true;
            }
            vcount_ = static_cast<uint16_t>(scanline_);
            if (was_in_visible_window && scanline_ == kLinesVisible) {
                ev.vblank_started = true;
            }
            if (vcount_ == vcount_compare) {
                ev.vcount_matched = true;
            }
        }
    }
    return ev;
}

uint32_t GbaPpu::cycles_until_next_event() const {
    uint32_t target_dot = (dot_in_scanline_ < kDotsVisible)
        ? kDotsVisible
        : kDotsPerScanline;
    uint32_t cycles = (target_dot - dot_in_scanline_) * kCyclesPerDot;
    if (cycles > cycle_in_dot_) cycles -= cycle_in_dot_;
    else cycles = 1;
    return cycles ? cycles : 1;
}

// ─────────────────────────────────────────────────────────────────────
// Renderer (Phase 2.4)
// ─────────────────────────────────────────────────────────────────────
//
// Scope: enough to draw the GBA BIOS boot intro. The BIOS intro uses
// DISPCNT = 0x1002 (BG mode 2, OBJ enabled, all BGs disabled), so we
// only need OBJ (sprite) compositing for this milestone.
//
// References: GBATEK § "GBA OBJs - OAM", § "GBA Palettes",
//             § "GBA VRAM Character Data".

namespace {

// Sprite shape × size → (width, height) in pixels.
//   shape: 0=square, 1=horizontal, 2=vertical
//   size:  0..3
constexpr int kSpriteWH[3][4][2] = {
    // square
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    // horizontal
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    // vertical
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
};

// Convert 16-bit GBA color (0BBBBBGGGGGRRRRR) to 24-bit RGB888.
inline void to_rgb888(uint16_t c, uint8_t* out) {
    // 5 → 8 bit expansion: ((v << 3) | (v >> 2)) gives full-range 0..255.
    uint8_t r = (c >>  0) & 0x1F;
    uint8_t g = (c >>  5) & 0x1F;
    uint8_t b = (c >> 10) & 0x1F;
    out[0] = static_cast<uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
    out[2] = static_cast<uint8_t>((b << 3) | (b >> 2));
}

inline uint16_t load_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint16_t blend_alpha_gba555(uint16_t top,
                                   uint16_t bottom,
                                   uint32_t eva,
                                   uint32_t evb) {
    if (eva > 16) eva = 16;
    if (evb > 16) evb = 16;
    const uint32_t tr = top & 31u;
    const uint32_t tg = ((top >> 4) & 62u) | (top >> 15);
    const uint32_t tb = (top >> 10) & 31u;
    const uint32_t br = bottom & 31u;
    const uint32_t bg = ((bottom >> 4) & 62u) | (bottom >> 15);
    const uint32_t bb = (bottom >> 10) & 31u;

    // The GBA blends in its native color domain, rounds to nearest (+8 before
    // the 4-bit division), and carries a sixth green precision bit in palette
    // bit 15. Only after blending is green reduced to the displayed 5 bits.
    // Doing this on already-expanded RGB888 values produces subtly wrong colors.
    const uint32_t r = std::min(31u, (tr * eva + br * evb + 8u) >> 4);
    const uint32_t g6 = std::min(63u, (tg * eva + bg * evb + 8u) >> 4);
    const uint32_t b = std::min(31u, (tb * eva + bb * evb + 8u) >> 4);
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

inline uint16_t brighten_gba555(uint16_t color, uint32_t evy) {
    if (evy > 16) evy = 16;
    uint32_t r = color & 31u;
    uint32_t g6 = ((color >> 4) & 62u) | (color >> 15);
    uint32_t b = (color >> 10) & 31u;
    r += ((31u - r) * evy + 8u) >> 4;
    g6 += ((63u - g6) * evy + 8u) >> 4;
    b += ((31u - b) * evy + 8u) >> 4;
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

inline uint16_t darken_gba555(uint16_t color, uint32_t evy) {
    if (evy > 16) evy = 16;
    uint32_t r = color & 31u;
    uint32_t g6 = ((color >> 4) & 62u) | (color >> 15);
    uint32_t b = (color >> 10) & 31u;
    // Hardware rounds the subtractive term with +7 rather than +8.
    r -= (r * evy + 7u) >> 4;
    g6 -= (g6 * evy + 7u) >> 4;
    b -= (b * evy + 7u) >> 4;
    return static_cast<uint16_t>((b << 10) | ((g6 >> 1) << 5) | r);
}

}  // namespace

// Read a signed 16-bit Q8.8 affine parameter from IO.
static inline int32_t read_s16(const uint8_t* io, uint32_t off) {
    int16_t v = static_cast<int16_t>(
        io[off] | (io[off + 1] << 8));
    return static_cast<int32_t>(v);
}

// Read a signed 28-bit Q19.8 reference coord from IO (BGxX/BGxY).
// Bits 27..0 are the value; the field is sign-extended from bit 27.
static inline int32_t read_s28_ref(const uint8_t* io, uint32_t off) {
    uint32_t v = static_cast<uint32_t>(io[off]) |
                 (static_cast<uint32_t>(io[off + 1]) <<  8) |
                 (static_cast<uint32_t>(io[off + 2]) << 16) |
                 (static_cast<uint32_t>(io[off + 3]) << 24);
    v &= 0x0FFFFFFFu;
    if (v & 0x08000000u) v |= 0xF0000000u;
    return static_cast<int32_t>(v);
}

static inline void write_s28_ref(uint8_t* io, uint32_t off, int32_t value) {
    const uint32_t v = static_cast<uint32_t>(value) & 0x0FFFFFFFu;
    io[off + 0] = static_cast<uint8_t>(v);
    io[off + 1] = static_cast<uint8_t>(v >> 8);
    io[off + 2] = static_cast<uint8_t>(v >> 16);
    io[off + 3] = static_cast<uint8_t>(v >> 24);
}

namespace {

bool obj_texel_opaque(const uint8_t* vram,
                      uint32_t obj_tile_base,
                      bool obj_1d_mapping,
                      uint32_t tile_num,
                      int tiles_w,
                      bool color256,
                      int tex_x,
                      int tex_y) {
    int tile_x_in_sprite = tex_x >> 3;
    int tile_y_in_sprite = tex_y >> 3;
    int px_in_tile       = tex_x & 7;
    int py_in_tile       = tex_y & 7;

    uint32_t this_tile;
    if (obj_1d_mapping) {
        this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                (color256 ? 2u : 1u);
    } else {
        this_tile = tile_num + (tile_y_in_sprite * 32u) +
                    tile_x_in_sprite * (color256 ? 2u : 1u);
    }
    uint32_t tile_off = obj_tile_base + this_tile * 32u;
    if (color256) {
        uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
        if (off + 1 > 96u * 1024u) return false;
        return vram[off] != 0;
    }
    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
    if (off + 1 > 96u * 1024u) return false;
    uint8_t b = vram[off];
    uint8_t pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
    return pal_index != 0;
}

void mark_obj_window_scanline(bool* mask,
                              uint32_t y,
                              uint16_t dispcnt,
                              const uint8_t* vram,
                              const uint8_t* oam,
                              uint32_t kScreenWidth,
                              uint32_t kScreenHeight) {
    if (y >= kScreenHeight) return;
    if ((dispcnt & 0x8000u) == 0) return;

    uint32_t bg_mode = dispcnt & 0x07u;
    constexpr uint32_t obj_tile_base = 0x10000u;
    bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;

    for (int idx = 127; idx >= 0; --idx) {
        const uint8_t* entry = oam + idx * 8;
        uint16_t attr0 = load_u16_le(&entry[0]);
        uint16_t attr1 = load_u16_le(&entry[2]);
        uint16_t attr2 = load_u16_le(&entry[4]);

        bool rot_scale = (attr0 & 0x0100u) != 0;
        bool disable_or_double = (attr0 & 0x0200u) != 0;
        if (!rot_scale && disable_or_double) continue;
        uint32_t obj_mode = (attr0 >> 10) & 0x3u;
        if (obj_mode != 2) continue;

        uint32_t shape = (attr0 >> 14) & 0x3u;
        if (shape >= 3) continue;
        uint32_t size  = (attr1 >> 14) & 0x3u;
        int sw = kSpriteWH[shape][size][0];
        int sh = kSpriteWH[shape][size][1];

        int sy = static_cast<int>(attr0 & 0xFFu);
        int sx = static_cast<int>(attr1 & 0x1FFu);
        if (sy >= 160) sy -= 256;
        if (sx & 0x100) sx -= 0x200;

        bool color256 = (attr0 & 0x2000u) != 0;
        uint32_t tile_num = attr2 & 0x3FFu;
        // Bitmap BGs consume the lower half of OBJ VRAM. Hardware preserves
        // the normal tile-number origin and ignores tiles 0..511 rather than
        // rebasing tile 0 to 0x14000.
        if (bg_mode >= 3 && tile_num < 512u) continue;
        int tiles_w = sw / 8;
        int tiles_h = sh / 8;

        if (rot_scale) {
            int bw = disable_or_double ? sw * 2 : sw;
            int bh = disable_or_double ? sh * 2 : sh;
            int j = static_cast<int>(y) - sy;
            if (j < 0 || j >= bh) continue;

            int affine_group = (attr1 >> 9) & 0x1Fu;
            const uint8_t* ag = oam + affine_group * 0x20u;
            int32_t pa = read_s16(ag, 0x06);
            int32_t pb = read_s16(ag, 0x0E);
            int32_t pc = read_s16(ag, 0x16);
            int32_t pd = read_s16(ag, 0x1E);

            int half_bw = bw >> 1;
            int half_bh = bh >> 1;
            int half_sw = sw >> 1;
            int half_sh = sh >> 1;
            int dy = j - half_bh;
            for (int i = 0; i < bw; ++i) {
                int screen_x = sx + i;
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                int dx = i - half_bw;
                int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                if (tex_x < 0 || tex_x >= sw) continue;
                if (tex_y < 0 || tex_y >= sh) continue;
                if (obj_texel_opaque(vram, obj_tile_base, obj_1d_mapping,
                                     tile_num, tiles_w, color256,
                                     tex_x, tex_y)) {
                    mask[screen_x] = true;
                }
            }
            continue;
        }

        int line = static_cast<int>(y) - sy;
        if (line < 0 || line >= sh) continue;
        bool hflip = (attr1 & 0x1000u) != 0;
        bool vflip = (attr1 & 0x2000u) != 0;
        int ty = line >> 3;
        int py = line & 7;
        int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
        int src_py = vflip ? (7 - py) : py;
        for (int tx = 0; tx < tiles_w; ++tx) {
            int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
            for (int px = 0; px < 8; ++px) {
                int screen_x = sx + tx * 8 + px;
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                int src_px = hflip ? (7 - px) : px;
                int tex_x = src_tx * 8 + src_px;
                int tex_y = src_ty * 8 + src_py;
                if (obj_texel_opaque(vram, obj_tile_base, obj_1d_mapping,
                                     tile_num, tiles_w, color256,
                                     tex_x, tex_y)) {
                    mask[screen_x] = true;
                }
            }
        }
    }
}

void render_scanline_internal(uint8_t* rgb,
                              uint32_t y,
                              uint16_t dispcnt,
                              const uint8_t* io,
                              const uint8_t* vram,
                              const uint8_t* oam,
                              const uint8_t* pal,
                              uint32_t kScreenWidth,
                              uint32_t kScreenHeight) {
    if (y >= kScreenHeight) return;

    uint8_t* row = rgb + y * kScreenWidth * 3;
    if (dispcnt & 0x0080u) {
        std::memset(row, 0xFF, kScreenWidth * 3);
        return;
    }

    struct PixelCandidate {
        uint8_t rgb[3] = {0, 0, 0};
        uint16_t color = 0;
        int key = 0x7FFFFFFF;
        uint8_t layer = 5;
        bool target1 = false;
        bool target2 = false;
        bool valid = false;
    };

    // ── Windows (GBATEK "LCD I/O Windows") ──────────────────────────────
    // window_control(x) returns the 6-bit per-pixel mask (BG0-3, OBJ, and bit5
    // = color-special-effect enable) chosen by region, priority WIN0 > WIN1 >
    // OBJ-window > outside. WIN0/WIN1 are COORDINATE windows: WINnH gives
    // [X1,X2), WINnV gives [Y1,Y2). The per-scanline WIN0H the game rewrites
    // each line is what carves the circular IRIS used by room transitions — it
    // blanks everything outside the circle to the backdrop, hiding the room's
    // VRAM/palette reload (MC-HP-003). With no window enabled, every layer is on.
    const bool win0_en   = (dispcnt & 0x2000u) != 0;
    const bool win1_en   = (dispcnt & 0x4000u) != 0;
    const bool objwin_en = (dispcnt & 0x8000u) != 0;
    const bool any_window = win0_en || win1_en || objwin_en;
    uint16_t winin  = static_cast<uint16_t>(io[0x48] | (io[0x49] << 8));
    uint16_t winout = static_cast<uint16_t>(io[0x4A] | (io[0x4B] << 8));
    // y within WINnV [Y1,Y2)? GBATEK: Y2>height or Y1>Y2 → Y2=height.
    auto win_v_row = [&](uint32_t vreg) -> bool {
        uint32_t v  = static_cast<uint32_t>(io[vreg] | (io[vreg + 1] << 8));
        uint32_t y1 = (v >> 8) & 0xFFu, y2 = v & 0xFFu;
        if (y2 > kScreenHeight || y1 > y2) y2 = kScreenHeight;
        return y >= y1 && y < y2;
    };
    // x within WINnH [X1,X2)? GBATEK: X2>width or X1>X2 → X2=width.
    auto win_h_in = [&](uint32_t hreg, uint32_t x) -> bool {
        uint32_t h  = static_cast<uint32_t>(io[hreg] | (io[hreg + 1] << 8));
        uint32_t x1 = (h >> 8) & 0xFFu, x2 = h & 0xFFu;
        if (x2 > kScreenWidth || x1 > x2) x2 = kScreenWidth;
        return x >= x1 && x < x2;
    };
    const bool win0_row = win0_en && win_v_row(0x44);
    const bool win1_row = win1_en && win_v_row(0x46);
    bool obj_window_storage[GbaPpu::kScreenWidth] = {};
    bool* obj_window_mask = nullptr;
    if (objwin_en) {
        mark_obj_window_scanline(obj_window_storage, y, dispcnt, vram, oam,
                                 kScreenWidth, kScreenHeight);
        obj_window_mask = obj_window_storage;
    }
    auto window_control = [&](uint32_t x) -> uint16_t {
        if (!any_window) return 0x3Fu;
        if (win0_row && win_h_in(0x40, x)) return winin & 0x3Fu;
        if (win1_row && win_h_in(0x42, x)) return (winin >> 8) & 0x3Fu;
        if (obj_window_mask && obj_window_mask[x])
            return static_cast<uint16_t>((winout >> 8) & 0x3Fu);
        return static_cast<uint16_t>(winout & 0x3Fu);
    };
    auto layer_enabled = [&](uint32_t x, uint32_t layer_bit) -> bool {
        return (window_control(x) & (1u << layer_bit)) != 0;
    };
    auto blend_enabled = [&](uint32_t x) -> bool {
        return (window_control(x) & (1u << 5)) != 0;
    };

    uint16_t bldcnt = static_cast<uint16_t>(io[0x50] | (io[0x51] << 8));
    uint16_t bldalpha = static_cast<uint16_t>(io[0x52] | (io[0x53] << 8));
    uint32_t first_targets = bldcnt & 0x3Fu;
    uint32_t second_targets = (bldcnt >> 8) & 0x3Fu;
    uint32_t effect = (bldcnt >> 6) & 0x3u;

    PixelCandidate top[GbaPpu::kScreenWidth];
    PixelCandidate second[GbaPpu::kScreenWidth];
    const uint16_t backdrop_color = load_u16_le(&pal[0]);
    uint8_t backdrop_rgb[3];
    to_rgb888(backdrop_color, backdrop_rgb);
    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        top[x].rgb[0] = backdrop_rgb[0];
        top[x].rgb[1] = backdrop_rgb[1];
        top[x].rgb[2] = backdrop_rgb[2];
        top[x].color = backdrop_color;
        top[x].key = 0x70000000;
        top[x].layer = 5;
        top[x].target1 = blend_enabled(x) && ((first_targets & (1u << 5)) != 0);
        top[x].target2 = (second_targets & (1u << 5)) != 0;
        top[x].valid = true;
    }
    auto submit = [&](uint32_t x,
                      uint16_t color,
                      int key,
                      uint8_t layer,
                      bool target1,
                      bool target2) {
        PixelCandidate cand;
        cand.color = color;
        to_rgb888(color, cand.rgb);
        cand.key = key;
        cand.layer = layer;
        cand.target1 = target1;
        cand.target2 = target2;
        cand.valid = true;
        if (key < top[x].key) {
            second[x] = top[x];
            top[x] = cand;
        } else if (key < second[x].key) {
            second[x] = cand;
        }
    };

    uint32_t bg_mode = dispcnt & 0x07u;
    auto render_regular_bg = [&](uint32_t layer,
                                 uint32_t cnt_off,
                                 uint32_t scroll_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;

        uint16_t bgcnt = static_cast<uint16_t>(
            io[cnt_off] | (io[cnt_off + 1] << 8));
        uint32_t char_base = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool color256 = (bgcnt & 0x0080u) != 0;
        uint32_t size_code = (bgcnt >> 14) & 0x3u;
        uint32_t bg_priority = bgcnt & 0x3u;
        uint32_t hofs = static_cast<uint16_t>(
            io[scroll_off] | (io[scroll_off + 1] << 8)) & 0x01FFu;
        uint32_t vofs = static_cast<uint16_t>(
            io[scroll_off + 2] | (io[scroll_off + 3] << 8)) & 0x01FFu;

        uint32_t width_tiles = (size_code & 1u) ? 64u : 32u;
        uint32_t height_tiles = (size_code & 2u) ? 64u : 32u;
        uint32_t width_px = width_tiles * 8u;
        uint32_t height_px = height_tiles * 8u;
        uint32_t block_cols = width_tiles / 32u;

        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            if (!layer_enabled(x, layer)) continue;
            uint32_t tex_x = (x + hofs) & (width_px - 1u);
            uint32_t tex_y = (y + vofs) & (height_px - 1u);
            uint32_t tile_x = tex_x >> 3;
            uint32_t tile_y = tex_y >> 3;
            uint32_t block = (tile_x >> 5) + (tile_y >> 5) * block_cols;
            uint32_t map_off = screen_base + block * 0x800u +
                ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u;
            if (map_off + 1 >= 96u * 1024u) continue;

            uint16_t entry = load_u16_le(&vram[map_off]);
            uint32_t tile_num = entry & 0x03FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            uint32_t palette_bank = (entry >> 12) & 0x0Fu;
            uint32_t px = tex_x & 7u;
            uint32_t py = tex_y & 7u;
            if (hflip) px = 7u - px;
            if (vflip) py = 7u - py;

            uint8_t pal_idx = 0;
            if (color256) {
                uint32_t tile_addr = char_base + tile_num * 64u + py * 8u + px;
                if (tile_addr >= 96u * 1024u) continue;
                pal_idx = vram[tile_addr];
            } else {
                uint32_t tile_addr = char_base + tile_num * 32u +
                    py * 4u + (px >> 1);
                if (tile_addr >= 96u * 1024u) continue;
                uint8_t packed = vram[tile_addr];
                pal_idx = (px & 1u) ? (packed >> 4) : (packed & 0x0Fu);
                pal_idx = static_cast<uint8_t>(
                    pal_idx | static_cast<uint8_t>(palette_bank << 4));
            }
            if ((pal_idx & (color256 ? 0xFFu : 0x0Fu)) == 0) continue;

            const uint16_t color = load_u16_le(&pal[pal_idx * 2]);
            submit(x, color,
                   static_cast<int>(bg_priority * 256u + 128u + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };

    auto render_affine_bg = [&](uint32_t layer,
                                uint32_t cnt_off,
                                uint32_t param_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;

        uint16_t bgcnt = static_cast<uint16_t>(io[cnt_off] |
                                               (io[cnt_off + 1] << 8));
        uint32_t char_base   = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool wrap            = (bgcnt & 0x2000u) != 0;
        uint32_t size_code   = (bgcnt >> 14) & 0x3u;
        int bg_pixels = 128 << size_code;
        int bg_tiles  = bg_pixels / 8;
        int bg_priority = static_cast<int>(bgcnt & 0x3u);

        int32_t pa = read_s16(io, param_off + 0x00);
        int32_t pb = read_s16(io, param_off + 0x02);
        int32_t pc = read_s16(io, param_off + 0x04);
        int32_t pd = read_s16(io, param_off + 0x06);
        int32_t refx = read_s28_ref(io, param_off + 0x08);
        int32_t refy = read_s28_ref(io, param_off + 0x0C);
        int32_t xt = refx + static_cast<int32_t>(y) * pb;
        int32_t yt = refy + static_cast<int32_t>(y) * pd;
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            int32_t tex_x = xt >> 8;
            int32_t tex_y = yt >> 8;
            xt += pa;
            yt += pc;
            if (!layer_enabled(x, layer)) continue;
            if (wrap) {
                tex_x &= (bg_pixels - 1);
                tex_y &= (bg_pixels - 1);
            } else if (tex_x < 0 || tex_x >= bg_pixels ||
                       tex_y < 0 || tex_y >= bg_pixels) {
                continue;
            }
            uint32_t map_off = screen_base + (tex_y >> 3) * bg_tiles + (tex_x >> 3);
            if (map_off >= 96u * 1024u) continue;
            uint8_t tile_index = vram[map_off];
            uint32_t tile_addr = char_base + tile_index * 64u +
                                 (tex_y & 7) * 8 + (tex_x & 7);
            if (tile_addr >= 96u * 1024u) continue;
            uint8_t pal_idx = vram[tile_addr];
            if (pal_idx == 0) continue;
            const uint16_t color = load_u16_le(&pal[pal_idx * 2]);
            submit(x, color,
                   static_cast<int>(bg_priority * 256 + 128 + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };

    // ── Bitmap BG modes 3/4/5 (GBATEK "LCD VRAM Bitmap BG Modes") ───────
    // All three present a single framebuffer through BG2's affine transform,
    // so the sampling loop mirrors render_affine_bg above:
    //   Mode 3  240x160, 16bpp BGR555 direct colour, one frame at 0x00000.
    //   Mode 4  240x160, 8bpp palette indices; frame 0 at 0x00000 and
    //           frame 1 at 0x0A000, selected by DISPCNT bit 4.
    //   Mode 5  160x128, 16bpp direct colour, same two frame bases.
    //
    // Transparency: mode 4 is palette-indexed, so index 0 is transparent
    // exactly like the tiled layers. Modes 3/5 are direct colour and have no
    // transparent index, so every in-bounds texel is opaque. Texels outside
    // the bitmap stay transparent and show the backdrop — that is what
    // letterboxes mode 5's 160x128 image inside the 240x160 screen.
    //
    // BG2CNT bit13 (display-area overflow / wraparound) is defined for the
    // TILED affine BGs; a bitmap is not a power-of-two tile map, so an
    // out-of-range texel here is transparent rather than wrapped.
    auto render_bitmap_bg = [&]() {
        if ((dispcnt & 0x0400u) == 0) return;   // DISPCNT bit10 = BG2 enable
        constexpr uint32_t layer = 2;
        uint16_t bgcnt = static_cast<uint16_t>(io[0x0C] | (io[0x0D] << 8));
        int bg_priority = static_cast<int>(bgcnt & 0x3u);

        const bool     direct = (bg_mode != 4);
        const int      bmp_w  = (bg_mode == 5) ? 160 : 240;
        const int      bmp_h  = (bg_mode == 5) ? 128 : 160;
        const uint32_t bpp    = direct ? 2u : 1u;
        const uint32_t frame_base =
            (bg_mode != 3 && (dispcnt & 0x0010u) != 0) ? 0xA000u : 0x0000u;

        int32_t pa   = read_s16(io, 0x20);
        int32_t pb   = read_s16(io, 0x22);
        int32_t pc   = read_s16(io, 0x24);
        int32_t pd   = read_s16(io, 0x26);
        int32_t refx = read_s28_ref(io, 0x28);
        int32_t refy = read_s28_ref(io, 0x2C);
        int32_t xt = refx + static_cast<int32_t>(y) * pb;
        int32_t yt = refy + static_cast<int32_t>(y) * pd;
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            int32_t tex_x = xt >> 8;
            int32_t tex_y = yt >> 8;
            xt += pa;
            yt += pc;
            if (!layer_enabled(x, layer)) continue;
            if (tex_x < 0 || tex_x >= bmp_w || tex_y < 0 || tex_y >= bmp_h)
                continue;
            const uint32_t off = frame_base +
                (static_cast<uint32_t>(tex_y) * static_cast<uint32_t>(bmp_w) +
                 static_cast<uint32_t>(tex_x)) * bpp;
            if (off + bpp > 96u * 1024u) continue;
            uint16_t color;
            if (direct) {
                color = load_u16_le(&vram[off]);
            } else {
                uint8_t pal_idx = vram[off];
                if (pal_idx == 0) continue;
                color = load_u16_le(&pal[pal_idx * 2]);
            }
            submit(x, color,
                   static_cast<int>(bg_priority * 256 + 128 + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };

    if (bg_mode == 0) {
        render_regular_bg(3, 0x0E, 0x1C);
        render_regular_bg(2, 0x0C, 0x18);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 1) {
        render_affine_bg(2, 0x0C, 0x20);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 2) {
        render_affine_bg(3, 0x0E, 0x30);
        render_affine_bg(2, 0x0C, 0x20);
    } else if (bg_mode <= 5) {
        // Modes 6/7 are prohibited (GBATEK); leave them backdrop-only.
        render_bitmap_bg();
    }

    if (dispcnt & 0x1000u) {
        constexpr uint32_t obj_tile_base = 0x10000u;
        bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;
        const uint8_t* obj_pal = pal + 0x200;
        for (int idx = 127; idx >= 0; --idx) {
            const uint8_t* entry = oam + idx * 8;
            uint16_t attr0 = load_u16_le(&entry[0]);
            uint16_t attr1 = load_u16_le(&entry[2]);
            uint16_t attr2 = load_u16_le(&entry[4]);
            bool rot_scale = (attr0 & 0x0100u) != 0;
            bool disable_or_double = (attr0 & 0x0200u) != 0;
            if (!rot_scale && disable_or_double) continue;
            uint32_t obj_mode = (attr0 >> 10) & 0x3u;
            if (obj_mode == 2 || obj_mode == 3) continue;
            uint32_t shape = (attr0 >> 14) & 0x3u;
            if (shape >= 3) continue;
            uint32_t size  = (attr1 >> 14) & 0x3u;
            int sw = kSpriteWH[shape][size][0];
            int sh = kSpriteWH[shape][size][1];
            int sy = static_cast<int>(attr0 & 0xFFu);
            int sx = static_cast<int>(attr1 & 0x1FFu);
            if (sy >= 160) sy -= 256;
            if (sx & 0x100) sx -= 0x200;
            bool color256 = (attr0 & 0x2000u) != 0;
            uint32_t tile_num = attr2 & 0x3FFu;
            if (bg_mode >= 3 && tile_num < 512u) continue;
            uint32_t palette_bank = (attr2 >> 12) & 0xFu;
            int tiles_w = sw / 8;
            int tiles_h = sh / 8;
            int priority = static_cast<int>((attr2 >> 10) & 0x3u);
            // Composite key (lower = front). Per-priority stride 256 with OBJ in
            // [p*256, p*256+127] (tie-break by OAM idx) and BG in [p*256+128,
            // p*256+131] (tie-break by layer) yields the exact GBA order
            // OBJ0<BG0<OBJ1<BG1<OBJ2<BG2<OBJ3<BG3: an OBJ is in front of a same-
            // priority BG, but a BG of priority p sits in front of any OBJ of
            // priority p+1 (this is what lets the player walk BEHIND roof/tree
            // tops). The stride MUST exceed 128 so OBJ idx (0..127) can't bleed
            // into the next priority's BG band.
            int key = priority * 256 + idx;
            bool obj_target2 = (second_targets & (1u << 4)) != 0;
            auto emit_obj = [&](int tex_x, int tex_y, int screen_x) {
                if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) return;
                if (!layer_enabled(static_cast<uint32_t>(screen_x), 4)) return;
                int tile_x_in_sprite = tex_x >> 3;
                int tile_y_in_sprite = tex_y >> 3;
                int px_in_tile = tex_x & 7;
                int py_in_tile = tex_y & 7;
                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
                } else {
                    this_tile = tile_num + (tile_y_in_sprite * 32u) +
                                tile_x_in_sprite * (color256 ? 2u : 1u);
                }
                uint32_t tile_off = obj_tile_base + this_tile * 32u;
                uint8_t pal_index;
                if (color256) {
                    uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                    if (off + 1 > 96u * 1024u) return;
                    pal_index = vram[off];
                    if (pal_index == 0) return;
                } else {
                    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                    if (off + 1 > 96u * 1024u) return;
                    uint8_t b = vram[off];
                    pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                    if (pal_index == 0) return;
                    pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
                }
                const uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
                uint32_t ux = static_cast<uint32_t>(screen_x);
                bool t1 = blend_enabled(ux) &&
                    obj_mode == 1;
                submit(ux, color, key, 4, t1, obj_target2);
            };
            if (rot_scale) {
                int bw = disable_or_double ? sw * 2 : sw;
                int bh = disable_or_double ? sh * 2 : sh;
                int j = static_cast<int>(y) - sy;
                if (j < 0 || j >= bh) continue;
                int affine_group = (attr1 >> 9) & 0x1Fu;
                const uint8_t* ag = oam + affine_group * 0x20u;
                int32_t pa = read_s16(ag, 0x06);
                int32_t pb = read_s16(ag, 0x0E);
                int32_t pc = read_s16(ag, 0x16);
                int32_t pd = read_s16(ag, 0x1E);
                int half_bw = bw >> 1;
                int half_bh = bh >> 1;
                int half_sw = sw >> 1;
                int half_sh = sh >> 1;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int dx = i - half_bw;
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;
                    emit_obj(tex_x, tex_y, sx + i);
                }
                continue;
            }
            int line = static_cast<int>(y) - sy;
            if (line < 0 || line >= sh) continue;
            bool hflip = (attr1 & 0x1000u) != 0;
            bool vflip = (attr1 & 0x2000u) != 0;
            int ty = line >> 3;
            int py = line & 7;
            int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
            int src_py = vflip ? (7 - py) : py;
            for (int tx = 0; tx < tiles_w; ++tx) {
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                for (int px = 0; px < 8; ++px) {
                    int src_px = hflip ? (7 - px) : px;
                    emit_obj(src_tx * 8 + src_px, src_ty * 8 + src_py,
                             sx + tx * 8 + px);
                }
            }
        }
    }

    // BLDY brightness coefficient (clamped to 16/16 = full).
    uint32_t bldy = static_cast<uint32_t>(io[0x54] | (io[0x55] << 8)) & 0x1Fu;
    if (bldy > 16u) bldy = 16u;
    for (uint32_t x = 0; x < kScreenWidth; ++x) {
        uint8_t* dst = row + x * 3;
        // Alpha blend top (1st target) with the layer below (2nd target). Per
        // GBATEK this occurs when effect==1 OR top is a semi-transparent OBJ
        // (mode 1 forces alpha regardless of BLDCNT). NOT gated on EVA!=0:
        // EVA=0/EVB=16 is a valid blend (1st target fully fades into the 2nd) —
        // the Oak-intro character fade endpoint that previously snapped back to
        // opaque, leaving body/feet (BG2 + semi-transparent OBJ) out of sync.
        if ((effect == 1 || top[x].layer == 4) &&
            top[x].target1 && second[x].valid && second[x].target2 &&
            !(top[x].layer == 4 && second[x].layer == 4)) {
            const uint16_t blended = blend_alpha_gba555(
                top[x].color, second[x].color,
                bldalpha & 0x1Fu, (bldalpha >> 8) & 0x1Fu);
            to_rgb888(blended, dst);
        } else if ((effect == 2u || effect == 3u) && bldy != 0u && top[x].target1) {
            // Brightness, like alpha, operates on native GBA channels before
            // RGB888 expansion and observes palette bit 15 as green precision.
            const uint16_t adjusted = effect == 2u
                ? brighten_gba555(top[x].color, bldy)
                : darken_gba555(top[x].color, bldy);
            to_rgb888(adjusted, dst);
        } else {
            dst[0] = top[x].rgb[0];
            dst[1] = top[x].rgb[1];
            dst[2] = top[x].rgb[2];
        }
    }
    return;
}

// ── Wide (view-expanded) scanline compositor ────────────────────────────────
// Renders output row `oy` using continuous hardware Y `hy` (= oy - extra_top).
// Output column x maps to hardware column hx = x - ox (ox = left margin).
// This path is ONLY entered when view-area expansion is active; the faithful
// build always takes render_scanline_internal above. For central hx in [0,240)
// the per-pixel logic matches vanilla; margin columns extrapolate BG/OBJ.
// WIN0/WIN1 H tests are not widened. Vertical: hy may be <0 or >=160 for peek
// / under-floor rows painted at VBlank; live HBlank still uses hy = vcount.
void render_scanline_wide(uint8_t* rgb, uint32_t oy, int32_t hy,
                          uint16_t dispcnt,
                          const uint8_t* io, const uint8_t* vram,
                          const uint8_t* oam, const uint8_t* pal,
                          uint32_t out_w, uint32_t ox) {
    // Layer1 is the left of this view. The follow measures the stock
    // point from Layer1 + extra, so gameplay samples 1:1 (ox = 0).
    // Title pillarbox still uses the margin. Columns left of the level
    // are skipped.
    constexpr uint32_t kVanW = GbaPpu::kScreenWidth;   // 240
    constexpr uint32_t kVanH = GbaPpu::kScreenHeight;  // 160 — native window V
    // oy is the framebuffer row; hy is continuous hardware Y (oy - extra_top).
    // Peek / under-floor rows use hy ∉ [0,160); native HBlank still uses hy = vcount.

    if (!g_ws_pillarbox && !g_ws_pillarbox_left && !g_ws_pillarbox_right)
        ox = 0;

    uint8_t* row = rgb + static_cast<std::size_t>(oy) * out_w * 3;
    if (dispcnt & 0x0080u) { std::memset(row, 0xFF, out_w * 3); return; }

    struct PixelCandidate {
        uint8_t rgb[3] = {0, 0, 0};
        uint16_t color = 0;
        int key = 0x7FFFFFFF;
        uint8_t layer = 5;
        bool target1 = false;
        bool target2 = false;
        bool valid = false;
    };

    const bool win0_en   = (dispcnt & 0x2000u) != 0;
    const bool win1_en   = (dispcnt & 0x4000u) != 0;
    const bool objwin_en = (dispcnt & 0x8000u) != 0;
    const bool any_window = win0_en || win1_en || objwin_en;
    uint16_t winin  = static_cast<uint16_t>(io[0x48] | (io[0x49] << 8));
    uint16_t winout = static_cast<uint16_t>(io[0x4A] | (io[0x4B] << 8));
    // The status plate is a screen-fixed layer. Its origin on the tall
    // view is the top row, centered on the 240 it was authored for.
    // A window that covers the native glass still covers the rows above
    // it, so the level there keeps drawing.
    const bool place_status =
        g_ws_extra_top != 0 && !g_ws_pillarbox &&
        !g_ws_pillarbox_left && !g_ws_pillarbox_right;
    const int status_x = place_status
        ? static_cast<int>((out_w - kVanW) / 2u)
        : 0;
    // The plate was one 240-wide strip. Lives and the score are its left
    // end, coins and the points its right end, and the star, the item box
    // and TIME sit between them. On the wide view the two ends go to the
    // edges and the middle stays on the center line.
    constexpr int kStatusLeftEnd = 50;
    constexpr int kStatusRightBegin = 180;
    auto status_native_x = [&](int x) -> int {
        if (!place_status) return x;
        if (x < kStatusLeftEnd) return x;
        const int right_out = static_cast<int>(out_w) - (static_cast<int>(kVanW) - kStatusRightBegin);
        if (x >= right_out) return kStatusRightBegin + (x - right_out);
        const int n = x - status_x;
        if (n >= kStatusLeftEnd && n < kStatusRightBegin) return n;
        return -1;
    };
    auto win_v_row = [&](uint32_t vreg) -> bool {
        uint32_t v  = static_cast<uint32_t>(io[vreg] | (io[vreg + 1] << 8));
        int y1 = static_cast<int>((v >> 8) & 0xFFu);
        int y2 = static_cast<int>(v & 0xFFu);
        // y2 == 0, or a bottom past the glass, is the bottom edge.
        if (y2 == 0 || y2 > static_cast<int>(kVanH) || y1 > y2)
            y2 = static_cast<int>(kVanH);
        if (y1 == 0)
            y1 = -static_cast<int>(g_ws_extra_top);
        if (y2 >= static_cast<int>(kVanH))
            y2 = static_cast<int>(kVanH) + static_cast<int>(g_ws_extra_bottom);
        return hy >= y1 && hy < y2;
    };
    auto win_h_in = [&](uint32_t hreg, int hx) -> bool {
        uint32_t h  = static_cast<uint32_t>(io[hreg] | (io[hreg + 1] << 8));
        int x1 = static_cast<int>((h >> 8) & 0xFFu), x2 = static_cast<int>(h & 0xFFu);
        if (x2 > static_cast<int>(kVanW) || x1 > x2) x2 = static_cast<int>(kVanW);
        return hx >= x1 && hx < x2;
    };
    const bool win0_row = win0_en && win_v_row(0x44);
    const bool win1_row = win1_en && win_v_row(0x46);
    // OBJ-window stencil is built in vanilla 240-space; margin columns (hx
    // outside [0,240)) get no OBJ-window (WINOUT), consistent with WIN0/1.
    bool obj_window_storage[GbaPpu::kScreenWidth] = {};
    bool* obj_window_mask = nullptr;
    if (objwin_en && hy >= 0 && hy < static_cast<int32_t>(kVanH)) {
        mark_obj_window_scanline(obj_window_storage, static_cast<uint32_t>(hy),
                                 dispcnt, vram, oam, kVanW, kVanH);
        obj_window_mask = obj_window_storage;
    }
    auto window_control_at_hx = [&](int hx) -> uint16_t {
        if (!any_window) return 0x3Fu;
        if (win0_row && win_h_in(0x40, hx)) return winin & 0x3Fu;
        if (win1_row && win_h_in(0x42, hx)) return (winin >> 8) & 0x3Fu;
        if (obj_window_mask && hx >= 0 && hx < static_cast<int>(kVanW) &&
            obj_window_mask[hx])
            return static_cast<uint16_t>((winout >> 8) & 0x3Fu);
        return static_cast<uint16_t>(winout & 0x3Fu);
    };
    // A guest-authored window has no defined continuation beyond the native
    // 240-pixel scanline. Extend it into the margins only when every authentic
    // pixel selects the same window control. Sampling just an edge and the
    // center misses narrow apertures (including OBJ-window stencils) that lie
    // between those probes and can leak scenery around transitions.
    uint16_t uniform_window_control = window_control_at_hx(0);
    bool window_control_is_uniform = true;
    if (g_ws_tilemap_provider && any_window) {
        for (int hx = 1; hx < static_cast<int>(kVanW); ++hx) {
            if (window_control_at_hx(hx) != uniform_window_control) {
                window_control_is_uniform = false;
                break;
            }
        }
    }
    const bool black_nonuniform_window_margins =
        g_ws_tilemap_provider && any_window && !window_control_is_uniform &&
        !g_ws_authored_margin_layers;
    auto window_control = [&](uint32_t x) -> uint16_t {
        int hx = static_cast<int>(x) - static_cast<int>(ox);
        if (g_ws_tilemap_provider &&
            (hx < 0 || hx >= static_cast<int>(kVanW))) {
            // Minish's room provider treats expanded columns as lying outside
            // every native WIN0/WIN1 rectangle, so WINOUT controls the world
            // there. Other providers retain the established fail-closed rule.
            // SMW/map WINOUT has no BG bits — extend the uniform in-window
            // control into margins when authored, else fail-closed.
            if (g_ws_authored_margin_layers)
                return window_control_is_uniform
                    ? uniform_window_control
                    : window_control_at_hx(hx);
            return window_control_is_uniform ? uniform_window_control : 0u;
        }
        return window_control_at_hx(hx);
    };
    // Every later compositor stage asks the same window questions for the
    // same output column. Resolve them once per scanline instead of repeating
    // WIN0/WIN1/OBJ-window branches for the backdrop, every BG, and every OBJ
    // pixel. The fixed-size stack array covers the renderer's clamped width.
    // These rows are ~16KB. On the guest stack that overflows after a few
    // minutes and the process dies with SIGSEGV. Keep them off that stack.
    static thread_local uint8_t window_controls[GbaPpu::kMaxRenderWidth];
    for (uint32_t x = 0; x < out_w; ++x)
        window_controls[x] = static_cast<uint8_t>(window_control(x) & 0x3Fu);
    auto layer_enabled = [&](uint32_t x, uint32_t layer_bit) -> bool {
        return (window_controls[x] & (1u << layer_bit)) != 0;
    };
    auto blend_enabled = [&](uint32_t x) -> bool {
        return (window_controls[x] & (1u << 5)) != 0;
    };

    uint16_t bldcnt = static_cast<uint16_t>(io[0x50] | (io[0x51] << 8));
    uint16_t bldalpha = static_cast<uint16_t>(io[0x52] | (io[0x53] << 8));
    uint32_t first_targets = bldcnt & 0x3Fu;
    uint32_t second_targets = (bldcnt >> 8) & 0x3Fu;
    uint32_t effect = (bldcnt >> 6) & 0x3u;

    static thread_local PixelCandidate top[GbaPpu::kMaxRenderWidth];
    static thread_local PixelCandidate second[GbaPpu::kMaxRenderWidth];
    const uint16_t backdrop_color = load_u16_le(&pal[0]);
    uint8_t backdrop_rgb[3];
    to_rgb888(backdrop_color, backdrop_rgb);
    for (uint32_t x = 0; x < out_w; ++x) {
        top[x].rgb[0] = backdrop_rgb[0];
        top[x].rgb[1] = backdrop_rgb[1];
        top[x].rgb[2] = backdrop_rgb[2];
        top[x].color = backdrop_color;
        top[x].key = 0x70000000;
        top[x].layer = 5;
        top[x].target1 = blend_enabled(x) && ((first_targets & (1u << 5)) != 0);
        top[x].target2 = (second_targets & (1u << 5)) != 0;
        top[x].valid = true;
        second[x].key = 0x7FFFFFFF;
        second[x].valid = false;
    }
    auto submit = [&](uint32_t x, uint16_t color, int key, uint8_t layer,
                      bool target1, bool target2) {
        PixelCandidate cand;
        cand.color = color;
        to_rgb888(color, cand.rgb);
        cand.key = key;
        cand.layer = layer;
        cand.target1 = target1;
        cand.target2 = target2;
        cand.valid = true;
        if (key < top[x].key) { second[x] = top[x]; top[x] = cand; }
        else if (key < second[x].key) { second[x] = cand; }
    };

    uint32_t bg_mode = dispcnt & 0x07u;
    auto render_regular_bg = [&](uint32_t layer, uint32_t cnt_off,
                                 uint32_t scroll_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;
        uint16_t bgcnt = static_cast<uint16_t>(io[cnt_off] | (io[cnt_off + 1] << 8));
        uint32_t char_base = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool color256 = (bgcnt & 0x0080u) != 0;
        uint32_t size_code = (bgcnt >> 14) & 0x3u;
        uint32_t bg_priority = bgcnt & 0x3u;
        uint32_t hofs = static_cast<uint16_t>(
            io[scroll_off] | (io[scroll_off + 1] << 8)) & 0x01FFu;
        uint32_t vofs = static_cast<uint16_t>(
            io[scroll_off + 2] | (io[scroll_off + 3] << 8)) & 0x01FFu;
        // One inset for every layer, shared with sprites below.
        const int layer_ox = static_cast<int>(ox);
        // HOFS stays the guest scroll. With ox = 0 that is the view.
        const int32_t hofs_i = static_cast<int32_t>(hofs);
        const int32_t vofs_i =
            static_cast<int32_t>(vofs) + g_ws_scroll_bias_y;
        uint32_t width_tiles = (size_code & 1u) ? 64u : 32u;
        uint32_t height_tiles = (size_code & 2u) ? 64u : 32u;
        uint32_t width_px = width_tiles * 8u;
        uint32_t height_px = height_tiles * 8u;
        uint32_t block_cols = width_tiles / 32u;
        // In a level the status plate is the screen-fixed layer: priority 0,
        // char base 1, screen base 28 (BGCNT 0xDC44). Its top band is the
        // bar. That band is drawn once, kStatusTopMargin rows below the top
        // of the tall view, split to the edges. Guest lines 0..kStatusBand
        // are that same bar; leaving them in place paints a second copy
        // over the level. Lines under that stay on the camera and the
        // window, so the water keeps its own position and is not split.
        // The overworld frame uses that same BGCNT. Room width is 0 off a
        // level, so none of this runs there.
        const bool status_bg = place_status &&
            g_ws_room_w > 0 &&
            screen_base == 28u * 0x800u &&
            char_base == 0x4000u &&
            bg_priority == 0;
        const int oy = hy + static_cast<int>(g_ws_extra_top);
        const int plate_y = oy - kStatusTopMargin;
        const bool status_layer = status_bg &&
            plate_y >= 0 && plate_y < kStatusBand;
        if (status_bg && !status_layer && hy >= 0 && hy < kStatusBand)
            return;
        // Rows above the camera are part of the tall view. A negative
        // sample is backdrop; wrapping that sample would pull the floor up.
        for (uint32_t x = 0; x < out_w; ++x) {
            int hx = static_cast<int>(x) - layer_ox;
            const bool authored_margin = !status_layer &&
                g_ws_authored_margin_layers &&
                (hx < 0 || hx >= static_cast<int>(kVanW));
            // Minish supplies room-map entries for margins independently of
            // the native 240px WIN0/WIN1 HUD/dialog masks. Ignore only their
            // regular-BG layer gate there; unsupported UI BGs still fail in
            // the provider below, so they cannot repeat.
            // The status ends now sit outside the native window. The plate
            // is screen-fixed, so the window does not get to hide them.
            if (!status_layer && !layer_enabled(x, layer) && !authored_margin) continue;
            int sample_hx = hx;
            int32_t tex_y_i = hy + vofs_i;
            if (status_layer) {
                sample_hx = status_native_x(static_cast<int>(x));
                if (sample_hx < 0 || sample_hx >= static_cast<int>(kVanW))
                    continue;
                tex_y_i = plate_y + static_cast<int32_t>(vofs);
            } else if (g_ws_bg_x_provider &&
                (g_ws_bg_x_provider_layers & (1u << layer))) {
                int provided_hx = hx;
                const int action = g_ws_bg_x_provider(
                    static_cast<int>(layer), static_cast<int>(x),
                    hy, &provided_hx);
                if (action < 0) continue;
                if (action > 0) sample_hx = provided_hx;
            }
            // Past the left or the top of the map: leave the backdrop.
            // Wrapping those samples pulls the far edge of the tilemap in.
            const int32_t world_x = sample_hx + hofs_i;
            if (world_x < 0) continue;
            uint32_t tex_x = static_cast<uint32_t>(world_x) &
                             (width_px - 1u);
            // Same rule as X. A negative coordinate is backdrop. The
            // rest of the map, including the rows above the glass, uses
            // this line's scroll. A second reject up there slices a
            // platform whose top sits in those rows.
            if (tex_y_i < 0) continue;
            uint32_t tex_y =
                static_cast<uint32_t>(tex_y_i) & (height_px - 1u);
            uint32_t tile_x = tex_x >> 3;
            uint32_t tile_y = tex_y >> 3;
            uint32_t block = (tile_x >> 5) + (tile_y >> 5) * block_cols;
            uint32_t map_off = screen_base + block * 0x800u +
                ((tile_y & 31u) * 32u + (tile_x & 31u)) * 2u;
            if (map_off + 1 >= 96u * 1024u) continue;
            uint16_t entry = load_u16_le(&vram[map_off]);
            // The level DMA writes one 256px screen at this base (080131C6).
            // Tile 0 and 0xF8 in that screen are real: 0 is the cave gap,
            // 0xF8 is a tileset cell. Replacing them paints the comb in
            // 080E0B58 across Donut Plains 2. Only the next screen, which
            // that DMA does not fill, is read from the decompressed map.
            if (screen_base == 0xC000u && tile_x >= 32u) {
                const uint32_t tn = entry & 0x03FFu;
                if (tn == 0u || tn == 0xF8u) {
                    uint16_t ext = 0;
                    if (smw_ws_mode0_bg_sample(tile_x, tile_y, &ext))
                        entry = ext;
                }
            }
            // Widescreen margin (hardware column outside 0..239): the 256px ring
            // entry here is the wrapped/aliased seam. If the sidecar can supply
            // the true off-screen world tile, use it; if it's armed but has no
            // data, leave the pixel transparent (no seam) rather than draw the
            // wrap. With no sidecar, keep vanilla wide behavior.
            if (sample_hx < 0 || sample_hx >= 240) {
                if (g_ws_tilemap_provider) {
                    uint16_t ext;
                    const int action = g_ws_tilemap_provider(
                        static_cast<int>(layer), sample_hx,
                        hy, &ext);
                    if (action == kWsTilemapReplace) {
                        entry = ext;
                    } else if (action != kWsTilemapKeepWrapped) {
                        continue;
                    }
                }
            }
            uint32_t tile_num = entry & 0x03FFu;
            bool hflip = (entry & 0x0400u) != 0;
            bool vflip = (entry & 0x0800u) != 0;
            uint32_t palette_bank = (entry >> 12) & 0x0Fu;
            uint32_t px = tex_x & 7u;
            uint32_t py = tex_y & 7u;
            if (hflip) px = 7u - px;
            if (vflip) py = 7u - py;
            uint8_t pal_idx = 0;
            if (color256) {
                uint32_t tile_addr = char_base + tile_num * 64u + py * 8u + px;
                if (tile_addr >= 96u * 1024u) continue;
                pal_idx = vram[tile_addr];
            } else {
                uint32_t tile_addr = char_base + tile_num * 32u + py * 4u + (px >> 1);
                if (tile_addr >= 96u * 1024u) continue;
                uint8_t packed = vram[tile_addr];
                pal_idx = (px & 1u) ? (packed >> 4) : (packed & 0x0Fu);
                pal_idx = static_cast<uint8_t>(
                    pal_idx | static_cast<uint8_t>(palette_bank << 4));
            }
            if ((pal_idx & (color256 ? 0xFFu : 0x0Fu)) == 0) continue;
            const uint16_t color = load_u16_le(&pal[pal_idx * 2]);
            submit(x, color,
                   static_cast<int>(bg_priority * 256u + 128u + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };
    auto render_affine_bg = [&](uint32_t layer, uint32_t cnt_off,
                                uint32_t param_off) {
        if ((dispcnt & (0x0100u << layer)) == 0) return;
        uint16_t bgcnt = static_cast<uint16_t>(io[cnt_off] | (io[cnt_off + 1] << 8));
        uint32_t char_base   = ((bgcnt >> 2) & 0x3u) * 0x4000u;
        uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
        bool wrap            = (bgcnt & 0x2000u) != 0;
        uint32_t size_code   = (bgcnt >> 14) & 0x3u;
        int bg_pixels = 128 << size_code;
        int bg_tiles  = bg_pixels / 8;
        int bg_priority = static_cast<int>(bgcnt & 0x3u);
        int32_t pa = read_s16(io, param_off + 0x00);
        int32_t pb = read_s16(io, param_off + 0x02);
        int32_t pc = read_s16(io, param_off + 0x04);
        int32_t pd = read_s16(io, param_off + 0x06);
        int32_t refx = read_s28_ref(io, param_off + 0x08);
        int32_t refy = read_s28_ref(io, param_off + 0x0C);
        // Extrapolate from the scanline reference using the signed logical x:
        // output column 0 is hardware x = -ox, so pre-advance by (-ox)*PA/PC.
        int32_t xt = refx + hy * pb +
                     static_cast<int32_t>(-static_cast<int>(ox)) * pa;
        int32_t yt = refy + hy * pd +
                     static_cast<int32_t>(-static_cast<int>(ox)) * pc;
        const bool filter =
            g_ws_affine_filter_enabled &&
            g_ws_affine_filter_provider &&
            g_ws_affine_filter_provider(
                static_cast<int>(layer), hy);
        auto sample_color = [&](int32_t tex_x, int32_t tex_y,
                                uint16_t* out_color) -> bool {
            if (wrap) {
                tex_x &= (bg_pixels - 1);
                tex_y &= (bg_pixels - 1);
            } else if (tex_x < 0 || tex_x >= bg_pixels ||
                       tex_y < 0 || tex_y >= bg_pixels) {
                return false;
            }
            const uint32_t map_off =
                screen_base + (tex_y >> 3) * bg_tiles + (tex_x >> 3);
            if (map_off >= 96u * 1024u) return false;
            const uint8_t tile_index = vram[map_off];
            const uint32_t tile_addr =
                char_base + tile_index * 64u +
                (tex_y & 7) * 8 + (tex_x & 7);
            if (tile_addr >= 96u * 1024u) return false;
            const uint8_t pal_idx = vram[tile_addr];
            if (pal_idx == 0) return false;
            *out_color = load_u16_le(&pal[pal_idx * 2]);
            return true;
        };
        for (uint32_t x = 0; x < out_w; ++x) {
            const int32_t sample_x = xt;
            const int32_t sample_y = yt;
            xt += pa;
            yt += pc;
            if (!layer_enabled(x, layer)) continue;
            const int32_t tex_x = sample_x >> 8;
            const int32_t tex_y = sample_y >> 8;
            uint16_t color = 0;
            if (!filter) {
                if (!sample_color(tex_x, tex_y, &color)) continue;
            } else {
                uint16_t c00 = 0, c10 = 0, c01 = 0, c11 = 0;
                if (!sample_color(tex_x, tex_y, &c00)) continue;
                if (sample_color(tex_x + 1, tex_y, &c10) &&
                    sample_color(tex_x, tex_y + 1, &c01) &&
                    sample_color(tex_x + 1, tex_y + 1, &c11)) {
                    const uint32_t fx =
                        static_cast<uint32_t>(sample_x) & 0xFFu;
                    const uint32_t fy =
                        static_cast<uint32_t>(sample_y) & 0xFFu;
                    const uint32_t ix = 256u - fx;
                    const uint32_t iy = 256u - fy;
                    const uint32_t w00 = ix * iy;
                    const uint32_t w10 = fx * iy;
                    const uint32_t w01 = ix * fy;
                    const uint32_t w11 = fx * fy;
                    auto blend_channel = [&](unsigned shift) {
                        const uint32_t sum =
                            ((c00 >> shift) & 31u) * w00 +
                            ((c10 >> shift) & 31u) * w10 +
                            ((c01 >> shift) & 31u) * w01 +
                            ((c11 >> shift) & 31u) * w11;
                        return (sum + 0x8000u) >> 16;
                    };
                    color = static_cast<uint16_t>(
                        blend_channel(0) |
                        (blend_channel(5) << 5) |
                        (blend_channel(10) << 10));
                } else {
                    color = c00;
                }
            }
            submit(x, color,
                   static_cast<int>(bg_priority * 256 + 128 + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };
    // Bitmap BG modes 3/4/5 — see the commentary on render_bitmap_bg in
    // render_scanline_internal above for the format/transparency rules.
    // A bitmap has a fixed width (240, or 160 in mode 5), so there is no
    // extra image data to reveal in the expanded margins: the affine
    // pre-advance by -ox places the bitmap exactly where it belongs and the
    // margins fall outside its bounds, staying backdrop.
    auto render_bitmap_bg = [&]() {
        if ((dispcnt & 0x0400u) == 0) return;   // DISPCNT bit10 = BG2 enable
        constexpr uint32_t layer = 2;
        uint16_t bgcnt = static_cast<uint16_t>(io[0x0C] | (io[0x0D] << 8));
        int bg_priority = static_cast<int>(bgcnt & 0x3u);

        const bool     direct = (bg_mode != 4);
        const int      bmp_w  = (bg_mode == 5) ? 160 : 240;
        const int      bmp_h  = (bg_mode == 5) ? 128 : 160;
        const uint32_t bpp    = direct ? 2u : 1u;
        const uint32_t frame_base =
            (bg_mode != 3 && (dispcnt & 0x0010u) != 0) ? 0xA000u : 0x0000u;

        int32_t pa   = read_s16(io, 0x20);
        int32_t pb   = read_s16(io, 0x22);
        int32_t pc   = read_s16(io, 0x24);
        int32_t pd   = read_s16(io, 0x26);
        int32_t refx = read_s28_ref(io, 0x28);
        int32_t refy = read_s28_ref(io, 0x2C);
        int32_t xt = refx + hy * pb +
                     static_cast<int32_t>(-static_cast<int>(ox)) * pa;
        int32_t yt = refy + hy * pd +
                     static_cast<int32_t>(-static_cast<int>(ox)) * pc;
        for (uint32_t x = 0; x < out_w; ++x) {
            int32_t tex_x = xt >> 8;
            int32_t tex_y = yt >> 8;
            xt += pa;
            yt += pc;
            if (!layer_enabled(x, layer)) continue;
            if (tex_x < 0 || tex_x >= bmp_w || tex_y < 0 || tex_y >= bmp_h)
                continue;
            const uint32_t off = frame_base +
                (static_cast<uint32_t>(tex_y) * static_cast<uint32_t>(bmp_w) +
                 static_cast<uint32_t>(tex_x)) * bpp;
            if (off + bpp > 96u * 1024u) continue;
            uint16_t color;
            if (direct) {
                color = load_u16_le(&vram[off]);
            } else {
                uint8_t pal_idx = vram[off];
                if (pal_idx == 0) continue;
                color = load_u16_le(&pal[pal_idx * 2]);
            }
            submit(x, color,
                   static_cast<int>(bg_priority * 256 + 128 + layer),
                   static_cast<uint8_t>(layer),
                   blend_enabled(x) && ((first_targets & (1u << layer)) != 0),
                   (second_targets & (1u << layer)) != 0);
        }
    };
    if (bg_mode == 0) {
        render_regular_bg(3, 0x0E, 0x1C);
        render_regular_bg(2, 0x0C, 0x18);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 1) {
        render_affine_bg(2, 0x0C, 0x20);
        render_regular_bg(1, 0x0A, 0x14);
        render_regular_bg(0, 0x08, 0x10);
    } else if (bg_mode == 2) {
        render_affine_bg(3, 0x0E, 0x30);
        render_affine_bg(2, 0x0C, 0x20);
    } else if (bg_mode <= 5) {
        // Modes 6/7 are prohibited (GBATEK); leave them backdrop-only.
        render_bitmap_bg();
    }

    if ((dispcnt & 0x1000u) && !g_ws_host_obj_compose) {
        constexpr uint32_t obj_tile_base = 0x10000u;
        bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;
        const uint8_t* obj_pal = pal + 0x200;
        for (int idx = 127; idx >= 0; --idx) {
            const uint8_t* entry = oam + idx * 8;
            uint16_t attr0 = load_u16_le(&entry[0]);
            uint16_t attr1 = load_u16_le(&entry[2]);
            uint16_t attr2 = load_u16_le(&entry[4]);
            bool rot_scale = (attr0 & 0x0100u) != 0;
            bool disable_or_double = (attr0 & 0x0200u) != 0;
            if (!rot_scale && disable_or_double) continue;
            uint32_t obj_mode = (attr0 >> 10) & 0x3u;
            if (obj_mode == 2 || obj_mode == 3) continue;
            uint32_t shape = (attr0 >> 14) & 0x3u;
            if (shape >= 3) continue;
            uint32_t size  = (attr1 >> 14) & 0x3u;
            int sw = kSpriteWH[shape][size][0];
            int sh = kSpriteWH[shape][size][1];
            int sy = static_cast<int>(attr0 & 0xFFu);
            // OAM X is normally signed 9-bit. An opted-in game may reinterpret
            // values that its widened guest writer emitted beyond hardware X.
            const int raw_sx = static_cast<int>(attr1 & 0x1FFu);
            int sx = raw_sx;
            // Y 160..255 folds to -96..-1. That is both a sprite clipping
            // the top of the glass and an object that fell past the floor.
            bool noted_y = false;
            if (g_ws_obj_y_provider &&
                g_ws_obj_y_provider(idx, attr0, &sy)) {
                // Full signed screen Y. Do not fold it at 160.
                noted_y = true;
            } else if (sy >= 160) {
                // 176..255 is a row above the glass (byte of a negative
                // Y). 160..175 is that same byte for a row just above the
                // picture, and also the band under the glass. The band
                // under the glass is extra_bottom rows; draw those. The
                // free-slot park is Y=160, X=0, tile 0, and that tile is
                // the face.
                const int above = 256 - static_cast<int>(g_ws_extra_top);
                const int bottom = 160 + static_cast<int>(g_ws_extra_bottom);
                if (sy >= above) sy -= 256;
                else if (sy < bottom) {
                    const unsigned tile = attr2 & 0x3FFu;
                    if (sy == 160 && raw_sx == 0 && tile == 0u) continue;
                } else {
                    continue;
                }
            }
            const int box_h = (rot_scale && disable_or_double) ? sh * 2 : sh;
            // The rows above the glass are part of the picture. A sprite
            // whose box sits entirely in them still draws. The same byte
            // is Mario once his real screen Y has gone past the floor;
            // that one stays under it.
            if (sy + box_h <= 0) {
                const int peek = static_cast<int>(g_ws_extra_top);
                // 192 is the guest's hide, not a sprite sitting on the
                // top row. A real row above the glass comes in as a note.
                if (!noted_y && (attr0 & 0xFFu) == 0xC0u) continue;
                // Drop the sprite only when its whole box is above the
                // picture. A tall sprite that starts above the extra rows
                // and ends inside them still draws, same as a map tile.
                if (peek <= 0 || sy + box_h <= -peek) continue;
                if (!noted_y && g_ws_player_screen_y >= 160) {
                    int wrapped = g_ws_player_screen_y & 0xFF;
                    if (wrapped >= 128) wrapped -= 256;
                    if (sy >= wrapped - 40 && sy <= wrapped + 16) continue;
                }
            }
            if (!(sy >= 0 && sy < 80))
                sy -= g_ws_scroll_bias_y;
            auto resolve_sx = [&] {
                int provided_sx = sx;
                if (g_ws_obj_attr_x_provider &&
                    g_ws_obj_attr_x_provider(idx, attr0, attr1, attr2,
                                             &provided_sx)) {
                    sx = provided_sx;
                } else if (g_ws_obj_x_provider &&
                    g_ws_obj_x_provider(raw_sx, &provided_sx)) {
                    sx = provided_sx;
                } else if (sx & 0x100) {
                    sx -= 0x200;
                }
            };
            bool color256 = (attr0 & 0x2000u) != 0;
            uint32_t tile_num = attr2 & 0x3FFu;
            if (bg_mode >= 3 && tile_num < 512u) continue;
            uint32_t palette_bank = (attr2 >> 12) & 0xFu;
            int tiles_w = sw / 8;
            int tiles_h = sh / 8;
            int priority = static_cast<int>((attr2 >> 10) & 0x3u);
            // Composite key (lower = front). Per-priority stride 256 with OBJ in
            // [p*256, p*256+127] (tie-break by OAM idx) and BG in [p*256+128,
            // p*256+131] (tie-break by layer) yields the exact GBA order
            // OBJ0<BG0<OBJ1<BG1<OBJ2<BG2<OBJ3<BG3: an OBJ is in front of a same-
            // priority BG, but a BG of priority p sits in front of any OBJ of
            // priority p+1 (this is what lets the player walk BEHIND roof/tree
            // tops). The stride MUST exceed 128 so OBJ idx (0..127) can't bleed
            // into the next priority's BG band.
            int key = priority * 256 + idx;
            bool obj_target2 = (second_targets & (1u << 4)) != 0;
            auto emit_obj = [&](int tex_x, int tex_y, int screen_x) {
                if (screen_x < 0 || screen_x >= static_cast<int>(out_w)) return;
                // The glass window ends at the old top. Rows above it are
                // the tall view; objects draw there with the tiles.
                if (hy >= 0 && !layer_enabled(static_cast<uint32_t>(screen_x), 4)) return;
                int tile_x_in_sprite = tex_x >> 3;
                int tile_y_in_sprite = tex_y >> 3;
                int px_in_tile = tex_x & 7;
                int py_in_tile = tex_y & 7;
                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
                } else {
                    this_tile = tile_num + (tile_y_in_sprite * 32u) +
                                tile_x_in_sprite * (color256 ? 2u : 1u);
                }
                uint32_t tile_off = obj_tile_base + this_tile * 32u;
                uint8_t pal_index;
                if (color256) {
                    uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                    if (off + 1 > 96u * 1024u) return;
                    pal_index = vram[off];
                    if (pal_index == 0) return;
                } else {
                    uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                    if (off + 1 > 96u * 1024u) return;
                    uint8_t b = vram[off];
                    pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                    if (pal_index == 0) return;
                    pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
                }
                const uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
                uint32_t ux = static_cast<uint32_t>(screen_x);
                bool t1 = blend_enabled(ux) && obj_mode == 1;
                submit(ux, color, key, 4, t1, obj_target2);
            };
            if (rot_scale) {
                int bw = disable_or_double ? sw * 2 : sw;
                int bh = disable_or_double ? sh * 2 : sh;
                int j = hy - sy;
                if (j < 0 || j >= bh) continue;
                resolve_sx();
                int affine_group = (attr1 >> 9) & 0x1Fu;
                const uint8_t* ag = oam + affine_group * 0x20u;
                int32_t pa = read_s16(ag, 0x06);
                int32_t pb = read_s16(ag, 0x0E);
                int32_t pc = read_s16(ag, 0x16);
                int32_t pd = read_s16(ag, 0x1E);
                int half_bw = bw >> 1;
                int half_bh = bh >> 1;
                int half_sw = sw >> 1;
                int half_sh = sh >> 1;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int dx = i - half_bw;
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;
                    emit_obj(tex_x, tex_y, sx + i + static_cast<int>(ox));
                }
                continue;
            }
            int line = hy - sy;
            if (line < 0 || line >= sh) continue;
            resolve_sx();
            bool hflip = (attr1 & 0x1000u) != 0;
            bool vflip = (attr1 & 0x2000u) != 0;
            int ty = line >> 3;
            int py = line & 7;
            int src_ty = vflip ? (tiles_h - 1 - ty) : ty;
            int src_py = vflip ? (7 - py) : py;
            for (int tx = 0; tx < tiles_w; ++tx) {
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                for (int px = 0; px < 8; ++px) {
                    int src_px = hflip ? (7 - px) : px;
                    emit_obj(src_tx * 8 + src_px, src_ty * 8 + src_py,
                             sx + tx * 8 + px + static_cast<int>(ox));
                }
            }
        }
    }

    uint32_t bldy = static_cast<uint32_t>(io[0x54] | (io[0x55] << 8)) & 0x1Fu;
    if (bldy > 16u) bldy = 16u;
    for (uint32_t x = 0; x < out_w; ++x) {
        uint8_t* dst = row + x * 3;
        // OBJ-only presentations (notably the real GBA BIOS logo) author the
        // whole backdrop through palette entry 0. There is no regular-BG
        // alias to protect in the margins, so preserve that authentic color
        // instead of replacing it with policy black; OBJ remains centered.
        const bool palette_backdrop_only = (dispcnt & 0x0F00u) == 0u;
        // Pillarbox policy: black out the margin columns on non-overworld
        // screens so menus/battles are letterboxed, not garbled.
        const bool left_margin = x < ox;
        const bool right_margin = x >= ox + 240u;
        const bool past_room =
            g_ws_room_w > 0 &&
            (g_ws_view_origin_x + static_cast<int>(x) >= g_ws_room_w);
        if (past_room ||
            (black_nonuniform_window_margins &&
             (left_margin || right_margin)) ||
            (!palette_backdrop_only &&
             ((g_ws_pillarbox && (left_margin || right_margin)) ||
              (g_ws_pillarbox_left && left_margin) ||
              (g_ws_pillarbox_right && right_margin)))) {
            dst[0] = dst[1] = dst[2] = 0;
            continue;
        }
        // Alpha blend top (1st target) with the layer below (2nd target). Per
        // GBATEK this occurs when effect==1 OR top is a semi-transparent OBJ
        // (mode 1 forces alpha regardless of BLDCNT). NOT gated on EVA!=0:
        // EVA=0/EVB=16 is a valid blend (1st target fully fades into the 2nd) —
        // the Oak-intro character fade endpoint that previously snapped back to
        // opaque, leaving body/feet (BG2 + semi-transparent OBJ) out of sync.
        if ((effect == 1 || top[x].layer == 4) &&
            top[x].target1 && second[x].valid && second[x].target2 &&
            !(top[x].layer == 4 && second[x].layer == 4)) {
            const uint16_t blended = blend_alpha_gba555(
                top[x].color, second[x].color,
                bldalpha & 0x1Fu, (bldalpha >> 8) & 0x1Fu);
            to_rgb888(blended, dst);
        } else if ((effect == 2u || effect == 3u) && bldy != 0u && top[x].target1) {
            const uint16_t adjusted = effect == 2u
                ? brighten_gba555(top[x].color, bldy)
                : darken_gba555(top[x].color, bldy);
            to_rgb888(adjusted, dst);
        } else {
            dst[0] = top[x].rgb[0];
            dst[1] = top[x].rgb[1];
            dst[2] = top[x].rgb[2];
        }
    }
}

}  // namespace

void GbaPpu::render(uint8_t* rgb,
                    uint16_t dispcnt,
                    const uint8_t* io,
                    const uint8_t* vram,
                    const uint8_t* oam,
                    const uint8_t* pal) const {
    if (!view_expanded()) {
        // Faithful path — literally unchanged from before view expansion existed.
        for (uint32_t y = 0; y < kScreenHeight; ++y) {
            render_scanline_internal(rgb, y, dispcnt, io, vram, oam, pal,
                                     kScreenWidth, kScreenHeight);
        }
        smw_ws_expire_obj_x();
        return;
    }
    const uint32_t ow = render_width();
    const uint32_t oh = render_height();
    // Continuous bottom-anchor: oy = hy + extra_top for every hy (peek and
    // native). Floor stays at the bottom; peek is real map above the camera.
    // Do NOT splice HUD/world scanlines — that shears the BG. HUD must move
    // as its own positioned elements, not by remapping the world FB.
    for (uint32_t oy = 0; oy < oh; ++oy) {
        const int32_t hy = static_cast<int32_t>(oy) -
                           static_cast<int32_t>(extra_top_);
        render_scanline_wide(rgb, oy, hy, dispcnt, io, vram, oam, pal,
                             ow, extra_left_);
    }
    smw_ws_expire_obj_x();
}

#if 0
    (void)oam;  // referenced below
    // Start with backdrop (palette entry 0) for every pixel. Backdrop
    // is the BG palette index 0, at PAL[0..1].
    uint16_t backdrop = load_u16_le(&pal[0]);
    uint8_t bd_rgb[3];
    to_rgb888(backdrop, bd_rgb);
    for (uint32_t y = 0; y < kScreenHeight; ++y) {
        for (uint32_t x = 0; x < kScreenWidth; ++x) {
            uint8_t* p = rgb + (y * kScreenWidth + x) * 3;
            p[0] = bd_rgb[0]; p[1] = bd_rgb[1]; p[2] = bd_rgb[2];
        }
    }

    // Forced blank (DISPCNT bit 7): output all-white per hardware.
    if (dispcnt & 0x0080u) {
        std::memset(rgb, 0xFF, kFramebufferBytes);
        return;
    }

    // Render background layers BEFORE OBJ so sprites composite on top.
    // Phase 2.5 scope: BG3 affine for BG mode 2 (GBA BIOS Nintendo
    // logo intro uses this). Other BG configurations land later.
    uint32_t bg_mode = dispcnt & 0x07u;
    bool bg3_enabled = (dispcnt & 0x0800u) != 0;
    if (bg3_enabled && (bg_mode == 1 || bg_mode == 2)) {
        // BG3 controls: BG3CNT at 0x0E, params at 0x30..0x3F.
        render_affine_bg(rgb, io, vram, pal,
                         0x0E, 0x30,
                         kScreenWidth, kScreenHeight);
    }

    // OBJ disabled?
    bool obj_enabled = (dispcnt & 0x1000u) != 0;
    if (!obj_enabled) return;

    // OBJ tile data starts at VRAM 0x10000 in tile modes (0/1/2) and
    // at VRAM 0x14000 in bitmap modes (3/4/5).
    uint32_t obj_tile_base = (bg_mode >= 3) ? 0x14000u : 0x10000u;
    bool obj_1d_mapping = (dispcnt & 0x0040u) != 0;

    // OBJ palette starts at PAL[0x200..0x3FF].
    const uint8_t* obj_pal = pal + 0x200;

    // Walk 128 OAM entries in priority-table order. For Phase 2.4 we
    // ignore priority bits and just back-to-front-draw so higher
    // OAM indices appear on top — close enough for the BIOS intro.
    for (int idx = 127; idx >= 0; --idx) {
        const uint8_t* entry = oam + idx * 8;
        uint16_t attr0 = load_u16_le(&entry[0]);
        uint16_t attr1 = load_u16_le(&entry[2]);
        uint16_t attr2 = load_u16_le(&entry[4]);

        bool rot_scale = (attr0 & 0x0100u) != 0;
        bool disable_or_double = (attr0 & 0x0200u) != 0;
        // For non-affine sprites bit 9 is the disable bit; for affine
        // sprites it's the double-size flag (handled in the affine
        // pass). Skip disabled non-affine sprites so the BIOS intro
        // screen blanks correctly once the BIOS clears the wordmark
        // by setting these bits.
        if (!rot_scale && disable_or_double) continue;

        // OBJ Mode (attr0 bits 10-11) per GBATEK § "GBA OBJs — OAM
        // Attribute 0":
        //   0 = Normal  (visible pixel)
        //   1 = Semi-Transparent (alpha-blend with BLDALPHA)
        //   2 = OBJ Window (the sprite's opaque pixels define a
        //       window region; sprite itself is NOT drawn)
        //   3 = Prohibited
        // Without this check the BIOS intro's OBJ-Window stencil
        // sprites leak into the visible frame as garbage pink pixels.
        uint32_t obj_mode = (attr0 >> 10) & 0x3u;
        if (obj_mode == 2) continue;            // window stencil — invisible
        if (obj_mode == 3) continue;            // prohibited
        // Semi-transparent (mode 1) renders as normal for now; proper
        // alpha blending lands when we wire BLDCNT / BLDALPHA.

        uint32_t shape = (attr0 >> 14) & 0x3u;
        if (shape >= 3) continue;
        uint32_t size  = (attr1 >> 14) & 0x3u;
        int sw = kSpriteWH[shape][size][0];
        int sh = kSpriteWH[shape][size][1];

        int sy = static_cast<int>(attr0 & 0xFFu);
        int sx = static_cast<int>(attr1 & 0x1FFu);
        // Y wraps at 256 (GBATEK).
        if (sy >= 160) sy -= 256;
        // X is 9 bits signed.
        if (sx & 0x100) sx -= 0x200;

        bool color256 = (attr0 & 0x2000u) != 0;
        uint32_t tile_num = attr2 & 0x3FFu;
        uint32_t palette_bank = (attr2 >> 12) & 0xFu;

        // For 1D mapping each row of tiles is contiguous in VRAM.
        // For 2D mapping each row of OBJ tiles is 32 tiles wide.
        int tiles_w = sw / 8;
        int tiles_h = sh / 8;

        // Texel sampler shared by affine + non-affine paths. Returns
        // false on transparent / out-of-VRAM, otherwise writes the
        // RGB888 pixel at (screen_x, screen_y).
        auto sample_and_emit = [&](int tex_x, int tex_y,
                                   int screen_x, int screen_y) {
            int tile_x_in_sprite = tex_x >> 3;
            int tile_y_in_sprite = tex_y >> 3;
            int px_in_tile       = tex_x & 7;
            int py_in_tile       = tex_y & 7;

            uint32_t this_tile;
            if (obj_1d_mapping) {
                this_tile = tile_num + (tile_y_in_sprite * tiles_w + tile_x_in_sprite) *
                                            (color256 ? 2u : 1u);
            } else {
                this_tile = tile_num + (tile_y_in_sprite * 32u) +
                            tile_x_in_sprite * (color256 ? 2u : 1u);
            }
            uint32_t tile_off = obj_tile_base + this_tile * 32u;

            uint8_t pal_index;
            if (color256) {
                uint32_t off = tile_off + py_in_tile * 8 + px_in_tile;
                if (off + 1 > 96u * 1024u) return;
                pal_index = vram[off];
                if (pal_index == 0) return;  // transparent
            } else {
                uint32_t off = tile_off + py_in_tile * 4 + (px_in_tile / 2);
                if (off + 1 > 96u * 1024u) return;
                uint8_t b = vram[off];
                pal_index = (px_in_tile & 1) ? (b >> 4) : (b & 0x0F);
                if (pal_index == 0) return;  // transparent
                pal_index = static_cast<uint8_t>(pal_index | (palette_bank << 4));
            }

            uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
            uint8_t* dst = rgb + (screen_y * kScreenWidth + screen_x) * 3;
            to_rgb888(color, dst);
        };

        if (rot_scale) {
            // Affine sprite. Bounding box is 2x the sprite size when
            // the double-size flag is set, otherwise = sprite size.
            int bw = disable_or_double ? sw * 2 : sw;
            int bh = disable_or_double ? sh * 2 : sh;

            int affine_group = (attr1 >> 9) & 0x1Fu;
            const uint8_t* ag = oam + affine_group * 0x20u;
            // PA/PB/PC/PD live at offsets 0x06, 0x0E, 0x16, 0x1E within
            // the 32-byte affine block (the other bytes belong to OBJ
            // attr0/1/2 entries that share the same 8-byte slots).
            int32_t pa = read_s16(ag, 0x06);
            int32_t pb = read_s16(ag, 0x0E);
            int32_t pc = read_s16(ag, 0x16);
            int32_t pd = read_s16(ag, 0x1E);

            int half_bw = bw >> 1;
            int half_bh = bh >> 1;
            int half_sw = sw >> 1;
            int half_sh = sh >> 1;

            for (int j = 0; j < bh; ++j) {
                int screen_y = sy + j;
                if (screen_y < 0 || screen_y >= static_cast<int>(kScreenHeight)) continue;
                int dy = j - half_bh;
                for (int i = 0; i < bw; ++i) {
                    int screen_x = sx + i;
                    if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                    int dx = i - half_bw;

                    // (tex_x, tex_y) = matrix * (dx, dy) + sprite_center.
                    int tex_x = ((pa * dx + pb * dy) >> 8) + half_sw;
                    int tex_y = ((pc * dx + pd * dy) >> 8) + half_sh;
                    if (tex_x < 0 || tex_x >= sw) continue;
                    if (tex_y < 0 || tex_y >= sh) continue;

                    sample_and_emit(tex_x, tex_y, screen_x, screen_y);
                }
            }
            continue;
        }

        // Non-affine sprite from here on.
        bool hflip = (attr1 & 0x1000u) != 0;
        bool vflip = (attr1 & 0x2000u) != 0;

        for (int ty = 0; ty < tiles_h; ++ty) {
            for (int tx = 0; tx < tiles_w; ++tx) {
                // Compute the source tile index, taking flips into
                // account (flipping the tile *layout* on top of
                // per-pixel flip).
                int src_tx = hflip ? (tiles_w - 1 - tx) : tx;
                int src_ty = vflip ? (tiles_h - 1 - ty) : ty;

                uint32_t this_tile;
                if (obj_1d_mapping) {
                    this_tile = tile_num + (src_ty * tiles_w + src_tx) *
                                                (color256 ? 2 : 1);
                } else {
                    // 2D mapping: the OBJ tile area is always 32
                    // *slots* wide regardless of color depth. Each
                    // 4bpp tile occupies 1 slot; each 8bpp tile
                    // occupies 2 horizontally-adjacent slots.
                    // Row stride in slot units is always 32.
                    this_tile = tile_num + (src_ty * 32u) +
                                src_tx * (color256 ? 2u : 1u);
                }
                // OBJ tile numbers are in 32-byte *slot* units
                // regardless of color depth. An 8bpp visible tile
                // (64 bytes) occupies 2 consecutive slots.
                uint32_t tile_off = obj_tile_base + this_tile * 32u;

                // Per-pixel emit.
                for (int py = 0; py < 8; ++py) {
                    int screen_y = sy + ty * 8 + py;
                    if (screen_y < 0 || screen_y >= static_cast<int>(kScreenHeight)) continue;
                    int src_py = vflip ? (7 - py) : py;
                    for (int px = 0; px < 8; ++px) {
                        int screen_x = sx + tx * 8 + px;
                        if (screen_x < 0 || screen_x >= static_cast<int>(kScreenWidth)) continue;
                        int src_px = hflip ? (7 - px) : px;

                        uint8_t pal_index;
                        if (color256) {
                            // 1 byte per pixel.
                            uint32_t off = tile_off + src_py * 8 + src_px;
                            if (off + 1 > 96 * 1024) continue;
                            pal_index = vram[off];
                        } else {
                            // 4bpp: 4 bytes per row, low nibble = even
                            // pixel, high nibble = odd pixel.
                            uint32_t off = tile_off + src_py * 4 + (src_px / 2);
                            if (off + 1 > 96 * 1024) continue;
                            uint8_t b = vram[off];
                            pal_index = (src_px & 1) ? (b >> 4) : (b & 0x0F);
                            if (pal_index == 0) continue;  // transparent
                            pal_index |= (palette_bank << 4);
                        }
                        if (color256 && pal_index == 0) continue;

                        uint16_t color = load_u16_le(&obj_pal[pal_index * 2]);
                        uint8_t* dst = rgb +
                            (screen_y * kScreenWidth + screen_x) * 3;
                        to_rgb888(color, dst);
                    }
                }
            }
        }
    }
}

#endif

void GbaPpu::render_scanline(uint32_t y,
                             uint16_t dispcnt,
                             const uint8_t* io,
                             const uint8_t* vram,
                             const uint8_t* oam,
                             const uint8_t* pal) {
    // BG2/BG3 affine coordinates are backed by hidden current-reference
    // registers. With constant PB/PD, ref + y*delta is equivalent. Mario Kart
    // HBlank-DMAs PA/PB/PC/PD/X/Y for every road scanline, however: each X/Y
    // write reloads the hidden reference and must be used directly rather than
    // multiplied by the absolute screen Y. Track the hardware accumulator and
    // rewrite a local IO image so the established compositor receives the
    // effective reference for this line through its ref + y*delta interface.
    std::array<uint8_t, 0x60> affine_io{};
    std::memcpy(affine_io.data(), io, affine_io.size());
    for (unsigned index = 0; index < affine_line_.size(); ++index) {
        AffineLineState& state = affine_line_[index];
        const uint32_t base = index == 0 ? 0x20u : 0x30u;
        const int32_t external_x = read_s28_ref(io, base + 0x08u);
        const int32_t external_y = read_s28_ref(io, base + 0x0Cu);
        if (y == 0 || !state.valid_x || state.reload_x) {
            state.x = external_x;
            state.valid_x = true;
        }
        if (y == 0 || !state.valid_y || state.reload_y) {
            state.y = external_y;
            state.valid_y = true;
        }
        state.reload_x = false;
        state.reload_y = false;

        const int32_t pb = read_s16(io, base + 0x02u);
        const int32_t pd = read_s16(io, base + 0x06u);
        write_s28_ref(affine_io.data(), base + 0x08u,
                      state.x - static_cast<int32_t>(y) * pb);
        write_s28_ref(affine_io.data(), base + 0x0Cu,
                      state.y - static_cast<int32_t>(y) * pd);
        state.x += pb;
        state.y += pd;
    }

    if (y == 0) {
        std::memcpy(line0_io_.data(), io, line0_io_.size());
        line0_io_valid_ = true;
        play_io_valid_ = false;
    }
    // Two metatiles under the status band. This line's scroll is the camera.
    if (y == 32u) {
        std::memcpy(play_io_.data(), io, play_io_.size());
        play_io_valid_ = true;
    }
    if (!view_expanded()) {
        render_scanline_internal(work_fb_.data(), y, dispcnt,
                                 affine_io.data(), vram, oam, pal,
                                 kScreenWidth, kScreenHeight);
        return;
    }
    // Live HBlank: native line y → FB row y+extra_top (bottom-anchored).
    const uint32_t oy = y + extra_top_;
    render_scanline_wide(work_fb_.data(), oy, static_cast<int32_t>(y),
                         dispcnt, affine_io.data(), vram, oam, pal,
                         render_width(), extra_left_);
}

void GbaPpu::paint_vertical_margins(uint16_t dispcnt,
                                    const uint8_t* io,
                                    const uint8_t* vram,
                                    const uint8_t* oam,
                                    const uint8_t* pal) {
    if (!view_expanded() || (extra_top_ == 0 && extra_bottom_ == 0)) return;
    const uint32_t ow = render_width();
    // Peek (hy < 0) and under-floor (hy >= 160): same continuous mapping.
    // Peek samples real scroll/map; g_ws_block_top_peek blocks inventing
    // wrapped floor tiles when the camera is already at the map top.
    // VBlank still holds the last line: the fall darken (BLDY) and the
    // bottom scroll. The rows above the glass continue line 0, where that
    // void is not applied.
    std::array<uint8_t, 0x60> peek_io{};
    const uint8_t* peek = io;
    uint16_t peek_dispcnt = dispcnt;
    if (line0_io_valid_) {
        // Line 0's blend, so the fall void is not painted across the sky.
        // Its scroll is the status row, not the camera. Keep the blend and
        // put the playfield layers back on the camera, or the rows above
        // the glass read the top of the tilemap: two metatiles of junk when
        // the camera is up there, and a missing platform when it is not.
        std::memcpy(peek_io.data(), line0_io_.data(), peek_io.size());
        if (play_io_valid_) {
            for (unsigned bg = 0; bg < 4u; ++bg) {
                const unsigned cnt = 0x08u + bg * 2u;
                const uint16_t bgcnt = static_cast<uint16_t>(
                    peek_io[cnt] | (static_cast<uint16_t>(peek_io[cnt + 1]) << 8));
                const uint32_t char_base = ((bgcnt >> 2) & 3u) * 0x4000u;
                const uint32_t screen_base = ((bgcnt >> 8) & 0x1Fu) * 0x800u;
                const bool status = g_ws_room_w > 0 &&
                                    screen_base == 28u * 0x800u &&
                                    char_base == 0x4000u &&
                                    (bgcnt & 3u) == 0u;
                if (status) continue;
                const unsigned off = 0x10u + bg * 4u;
                std::memcpy(peek_io.data() + off, play_io_.data() + off, 4u);
            }
        }
        peek = peek_io.data();
        peek_dispcnt = static_cast<uint16_t>(
            peek_io[0] | (static_cast<uint16_t>(peek_io[1]) << 8));
    }
    for (uint32_t oy = 0; oy < extra_top_; ++oy) {
        const int32_t hy = static_cast<int32_t>(oy) -
                           static_cast<int32_t>(extra_top_);
        render_scanline_wide(work_fb_.data(), oy, hy, peek_dispcnt, peek,
                             vram, oam, pal, ow, extra_left_);
    }
    for (uint32_t i = 0; i < extra_bottom_; ++i) {
        const uint32_t oy = extra_top_ + kScreenHeight + i;
        const int32_t hy = static_cast<int32_t>(kScreenHeight + i);
        render_scanline_wide(work_fb_.data(), oy, hy, dispcnt, io, vram, oam,
                             pal, ow, extra_left_);
    }
}

void GbaPpu::apply_status_ui_pin(uint16_t /*dispcnt*/,
                                 const uint8_t* /*io*/,
                                 const uint8_t* /*vram*/,
                                 const uint8_t* /*oam*/,
                                 const uint8_t* /*pal*/) {
    // Disabled: scanline pin shears BG. SMW uses host stamp-detach instead.
}

void GbaPpu::note_affine_reference_write(unsigned bg, bool y_axis) {
    if (bg < 2 || bg > 3) return;
    AffineLineState& state = affine_line_[bg - 2];
    if (y_axis) state.reload_y = true;
    else state.reload_x = true;
}

void GbaPpu::latch_framebuffer(uint16_t dispcnt,
                               const uint8_t* io,
                               const uint8_t* vram,
                               const uint8_t* oam,
                               const uint8_t* pal) {
    render(work_fb_.data(), dispcnt, io, vram, oam, pal);
    mark_framebuffer_latched();
}

void GbaPpu::mark_framebuffer_latched() {
    std::memcpy(latched_fb_.data(), work_fb_.data(), render_bytes());
    has_latched_fb_ = true;
    if (g_ws_post_latch_hook) {
        g_ws_post_latch_hook(latched_fb_.data(), render_width(),
                             render_height(), extra_left_, extra_top_);
    }
}

}  // namespace gba
