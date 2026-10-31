// gba_ppu.h — GBA PPU minimal state.
//
// Phase 2.3 scope: enough PPU to advance VCOUNT and the DISPSTAT
// VBlank/HBlank/VCount-match flags. Real rendering (BG modes,
// sprites, blending, etc.) lands in Phase 2.4+.
//
// Cycle accounting per GBATEK § "GBA Picture Processing Unit":
//   1 dot           = 4 cycles
//   HDraw window    = 252 dots (1008 cycles) per scanline
//   HBlank window   = 56 dots  (224 cycles) per scanline
//   scanline total  = 308 dots / 1232 cycles
//   visible lines   = 160
//   VBlank lines    = 68 (scanlines 160..227)
//   frame total     = 228 scanlines = 280896 cycles
//
// VCOUNT increments at the start of each scanline. HBlank flag sets
// at dot 252 (cycle 1008 of the line), clears at start of next line.

#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace gbarecomp::debug { class SnapshotWriter; class SnapshotReader; }

namespace gba {

class GbaPpu {
public:
    // Hardware constants.
    static constexpr uint32_t kDotsVisible      = 252;
    static constexpr uint32_t kDotsHBlank       = 56;
    static constexpr uint32_t kDotsPerScanline  = kDotsVisible + kDotsHBlank;  // 308
    static constexpr uint32_t kCyclesPerDot     = 4;
    static constexpr uint32_t kCyclesPerScanline =
        kDotsPerScanline * kCyclesPerDot;                                       // 1232
    static constexpr uint32_t kLinesVisible     = 160;
    static constexpr uint32_t kLinesTotal       = 228;
    static constexpr uint32_t kCyclesPerFrame   =
        kLinesTotal * kCyclesPerScanline;                                       // 280896

    GbaPpu();
    ~GbaPpu();

    // Events that fired during a tick. The caller routes these to
    // the IRQ controller (gating on the DISPSTAT enable bits the BIOS
    // sets). Keeping events as a return value rather than a callback
    // keeps the PPU pure / portable.
    struct TickEvents {
        bool vblank_started = false;  // scanline crossed from 159 → 160
        bool hblank_started = false;  // dot crossed visible → HBlank
        bool vcount_matched = false;  // scanline became == compare value
        bool frame_completed = false; // scanline wrapped 227 → 0
    };

    // Advance the PPU by `cycles` host cycles. Increments dot/scanline
    // counters and toggles VBlank/HBlank flags appropriately. The
    // caller (scheduler) accumulates the cycle budget per CPU step.
    //
    // `vcount_compare` is the value DISPSTAT[15:8] specifies; we
    // surface VCount-match in the returned events.
    TickEvents tick(uint32_t cycles, uint16_t vcount_compare = 0xFFFFu);

    // Live state for IO reads.
    uint16_t vcount() const { return static_cast<uint16_t>(vcount_); }
    bool     in_vblank() const { return scanline_ >= kLinesVisible; }
    bool     in_hblank() const { return dot_in_scanline_ >= kDotsVisible; }
    uint32_t cycles_until_next_event() const;

    // VCount-match: returns true when current VCOUNT equals the value
    // configured in DISPSTAT bits 8..15 (the IO layer holds that
    // value; we just expose vcount() for the comparison).
    // For Phase 2.3 the bus owns the comparison; this stays simple.

    // Reset to scanline 0, dot 0. Used by the runtime on cold boot.
    void reset();

    // Save-state serialization (all internal timing + latched frame).
    // No live pointers to skip. See debug/snapshot.h.
    void serialize(gbarecomp::debug::SnapshotWriter& w) const;
    void deserialize(gbarecomp::debug::SnapshotReader& r);

    // Total frames completed since reset — useful for sync points.
    uint64_t frame_count() const { return frame_count_; }

    // Render the current frame into a 240*160 RGB888 buffer.
    // Phase 2.4 scope: OBJ (sprite) rendering for non-affine
    // sprites only. BG layers and affine sprites land in 2.5+.
    static constexpr uint32_t kScreenWidth  = 240;
    static constexpr uint32_t kScreenHeight = 160;
    static constexpr std::size_t kFramebufferBytes =
        kScreenWidth * kScreenHeight * 3;

    // ── View-area expansion (opt-in enhancement; default OFF = faithful) ──
    // Generic "render extra margin columns/rows" runner capability. With every
    // margin 0 the PPU runs the LITERAL vanilla path (render_scanline_internal),
    // so OFF-mode is byte-identical to the faithful build BY CONSTRUCTION — not
    // by algebraic equivalence (per the widescreen design review). Horizontal
    // widening reuses each scanline's latched register state; VERTICAL widening
    // invents peek/under-floor rows (hy = oy - extra_top) with the same
    // compositor. kMaxExtraY caps per-side peek.
    // These margins are present-time host state and are NEVER serialized into
    // the snapshot (the save format is unchanged).
    // Experimental horizontal envelope: 120 pixels per side gives an exact
    // 480x160 maximum while the default remains the literal 240x160 path.
    static constexpr uint32_t kMaxExtraX = 120;
    static constexpr uint32_t kMaxExtraY = 80;
    static constexpr uint32_t kMaxRenderWidth  = kScreenWidth  + 2u * kMaxExtraX;  // 480
    static constexpr uint32_t kMaxRenderHeight = kScreenHeight + 2u * kMaxExtraY;  // 320
    static constexpr std::size_t kMaxFramebufferBytes =
        static_cast<std::size_t>(kMaxRenderWidth) * kMaxRenderHeight * 3;

    // Set the per-side view margins (clamped to the compile-time max per axis).
    // Call once at runtime init from the runner's --widescreen wiring.
    void set_view_margins(uint32_t left, uint32_t right,
                          uint32_t top, uint32_t bottom);
    uint32_t view_extra_left()   const { return extra_left_; }
    uint32_t view_extra_right()  const { return extra_right_; }
    uint32_t view_extra_top()    const { return extra_top_; }
    uint32_t view_extra_bottom() const { return extra_bottom_; }
    bool     view_expanded()     const {
        return (extra_left_ | extra_right_ | extra_top_ | extra_bottom_) != 0;
    }
    // Active output dimensions = vanilla + active margins (== 240/160 when OFF).
    uint32_t render_width()  const { return kScreenWidth  + extra_left_ + extra_right_; }
    uint32_t render_height() const { return kScreenHeight + extra_top_ + extra_bottom_; }
    std::size_t render_bytes() const {
        return static_cast<std::size_t>(render_width()) * render_height() * 3;
    }

    void render(uint8_t* rgb,
                uint16_t dispcnt,
                const uint8_t* io,    // 1 KB IO page for BGxCNT, BGxX/Y/PA/PB/PC/PD
                const uint8_t* vram,
                const uint8_t* oam,
                const uint8_t* pal) const;
    void render_scanline(uint32_t y,
                         uint16_t dispcnt,
                         const uint8_t* io,
                         const uint8_t* vram,
                         const uint8_t* oam,
                         const uint8_t* pal);

    // Invent peek / under-floor rows (hy∉[0,160)) into work_fb_. Call once
    // per frame at VBlank start before mark_framebuffer_latched().
    void paint_vertical_margins(uint16_t dispcnt,
                                const uint8_t* io,
                                const uint8_t* vram,
                                const uint8_t* oam,
                                const uint8_t* pal);

    // After continuous / HBlank compose: pin guest status hy∈[0,band) to
    // oy=hy and refill mid from peek. Operates on work_fb_.
    void apply_status_ui_pin(uint16_t dispcnt,
                             const uint8_t* io,
                             const uint8_t* vram,
                             const uint8_t* oam,
                             const uint8_t* pal);

    // The affine reference registers have an internal scanline accumulator.
    // Any write to BG2X/Y or BG3X/Y reloads that accumulator immediately;
    // games may DMA new references every HBlank. GbaIo calls this after the
    // backing register byte/halfword is committed.
    void note_affine_reference_write(unsigned bg, bool y_axis);

    // Latch the just-finished visible frame. The BIOS mutates OAM/PAL
    // during VBlank; screenshots must therefore use the frame captured
    // at VBlank start, not whatever live memory contains later.
    void latch_framebuffer(uint16_t dispcnt,
                           const uint8_t* io,
                           const uint8_t* vram,
                           const uint8_t* oam,
                           const uint8_t* pal);
    void mark_framebuffer_latched();
    const uint8_t* latched_framebuffer() const { return latched_fb_.data(); }
    bool has_latched_framebuffer() const { return has_latched_fb_; }

private:
    uint32_t scanline_        = 0;   // 0..227
    uint32_t dot_in_scanline_ = 0;   // 0..307 (in dots, not cycles)
    uint32_t cycle_in_dot_    = 0;   // 0..3
    uint16_t vcount_          = 0;
    uint64_t frame_count_     = 0;
    // Oversized to the compile-time max so the widened frame fits without
    // reallocation; only the first render_bytes() are used (vanilla = 115200).
    // work_fb_ is the scanline compositor's target: render_scanline() writes
    // each line here as the PPU reaches it, so mid-frame it holds a mix of the
    // frame in progress (top) and the previous frame (bottom). latched_fb_
    // only ever holds COMPLETE frames: mark_framebuffer_latched() snapshots
    // work_fb_ at VBlank start. Consumers (present, framedump, TCP screenshot)
    // read latched_fb_ at arbitrary guest-time points — handing them work_fb_
    // directly produced a horizontal one-frame-stale tear band at whatever
    // scanline the guest happened to have rendered to (the Minish Cap
    // walk-scroll "warble").
    std::array<uint8_t, kMaxFramebufferBytes> work_fb_{};
    std::array<uint8_t, kMaxFramebufferBytes> latched_fb_{};
    bool has_latched_fb_ = false;

    struct AffineLineState {
        int32_t x = 0;
        int32_t y = 0;
        bool valid_x = false;
        bool valid_y = false;
        bool reload_x = false;
        bool reload_y = false;
    };
    std::array<AffineLineState, 2> affine_line_{};
    // HDMA walks blend and scroll down the 160 lines. By VBlank, BLDY holds
    // the darkness of the last line — the fall void. The rows above the
    // glass keep line 0's image, or that void is painted across the sky.
    std::array<uint8_t, 0x60> line0_io_{};
    bool line0_io_valid_ = false;
    // Line 0 is the status row. Its BG scroll is not the camera: the tall
    // peek was sampling that row, so the top two metatiles wrapped to junk
    // and a platform sitting just above the glass never landed on its tiles.
    // Captured once the scan is past that band.
    std::array<uint8_t, 0x60> play_io_{};
    bool play_io_valid_ = false;

    // View-area margins (present-time host state; NOT serialized — see above).
    uint32_t extra_left_   = 0;
    uint32_t extra_right_  = 0;
    uint32_t extra_top_    = 0;
    uint32_t extra_bottom_ = 0;
};

// Widescreen margin tilemap provider (Step C; see gba_ppu.cpp). Set by the
// runtime-side sidecar; nullptr = vanilla wide behavior.
// Result 0 means no authored continuation, 1 replaces the wrapped hardware
// entry through out_entry, and 2 explicitly accepts the wrapped entry for a
// deliberately tiled effect rather than room geometry.
enum WsTilemapProviderResult : int {
    kWsTilemapUnavailable = 0,
    kWsTilemapReplace = 1,
    kWsTilemapKeepWrapped = 2,
};
extern "C" int (*g_ws_tilemap_provider)(int bg, int hw_x, int screen_y,
                                        uint16_t* out_entry);
// Optional per-game presentation remap for regular BG samples in the expanded
// renderer. The callback receives the physical output X and may suppress the
// sample (-1), leave the hardware X unchanged (0), or provide an authentic
// hardware X through out_hw_x (1). The native 240x160 renderer never calls it.
extern "C" int (*g_ws_bg_x_provider)(int bg, int output_x, int screen_y,
                                     int* out_hw_x);
// Bitmask of regular BG layers that may use g_ws_bg_x_provider. Defaults to
// all layers; game adapters can narrow it to avoid per-pixel callback traffic.
extern "C" unsigned g_ws_bg_x_provider_layers;
// Optional enhancement for affine backgrounds in expanded rendering. The
// global switch is presentation configuration; the provider lets a game
// authorize only known-safe layers and scenes.
extern "C" int g_ws_affine_filter_enabled;
extern "C" int (*g_ws_affine_filter_provider)(int bg, int screen_y);
// Per-game policy for self-sufficient authored margin providers. Off keeps the
// established fail-closed window/savestate behavior. On lets provider-sourced
// regular BG margins continue independently beside native HUD/dialog windows.
extern "C" int g_ws_authored_margin_layers;
extern "C" int g_ws_pillarbox;  // Step C policy: black margins on non-field screens
extern "C" int g_ws_pillarbox_left;
extern "C" int g_ws_pillarbox_right;
extern "C" int g_ws_scroll_bias_x;
extern "C" int g_ws_scroll_bias_y;
// Guest Layer1 minus the held lock. The wide renderer pulls the picture back
// onto that line; 0 leaves the guest scroll alone.
extern "C" int g_ws_cam_correction;
extern "C" int g_ws_cam_guest_l1;
extern "C" int g_ws_view_origin_x;
extern "C" int g_ws_room_w;
extern "C" int g_ws_block_top_peek;
extern "C" int g_ws_player_screen_x;
extern "C" int g_ws_player_screen_y;
extern "C" unsigned g_ws_extra_top;
extern "C" unsigned g_ws_extra_bottom;
// Rows of sky above the status plate. The bar was flush with the top of
// the tall view; this inset drops the plate, and the reserve icon with it.
constexpr int kStatusTopMargin = 4;
// The bar and the reserve item, in guest lines. Line 32 is already the
// camera. Those lines are drawn once, at the top of the tall view.
constexpr int kStatusBand = 32;
// When nonzero, text-BG fetch in hy∈[0,status) blanks non-scenery tiles
// so a second pass can erase the mid-screen status stamp after pin-to-top.
extern "C" int g_ws_status_tile_scrub;
// When nonzero and extra_top>0: after continuous render, redraw guest hy∈[0,band)
// at oy=hy (status UI at FB top) and refill oy=hy+extra_top from peek
// (hy-extra_top). Redraw from PPU state — not an FB row copy.
extern "C" int g_ws_status_ui_pin;
// When nonzero, render_scanline_wide skips guest OAM world sprites; the game's
// post-latch host layer owns vertical placement (int32 display-list Y).
extern "C" int g_ws_host_obj_compose;
// Optional per-game interpretation of the 9-bit OBJ X field in expanded-view
// rendering. Hardware-faithful signed decoding remains the default.
extern "C" int (*g_ws_obj_x_provider)(int raw_x, int* out_x);
// Richer per-game OBJ placement hook for screen-space UI. Called only by the
// expanded renderer and before g_ws_obj_x_provider; receives the OAM index and
// raw attributes so a game can distinguish HUD sprites from world objects.
extern "C" int (*g_ws_obj_attr_x_provider)(int oam_index,
                                            std::uint16_t attr0,
                                            std::uint16_t attr1,
                                            std::uint16_t attr2,
                                            int* out_x);
// Full signed screen Y for the same OAM slot. The guest stores only the
// low byte; without this, Y ≥ 160 is folded by −256 into the playfield.
extern "C" int (*g_ws_obj_y_provider)(int oam_index, std::uint16_t attr0,
                                      int* out_y);
// Host post-latch compose (SMW sprite/HUD layers). nullptr = no extra pass.
extern "C" void (*g_ws_post_latch_hook)(std::uint8_t* fb, std::uint32_t w,
                                         std::uint32_t h,
                                         std::uint32_t extra_left,
                                         std::uint32_t extra_top);

}  // namespace gba
