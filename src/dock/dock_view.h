// DockForge - Dock tile model, geometry and painter (T-009, hover T-010).
//
// Split from main.cpp because the Dock is the product: the host window is
// scaffolding around it. Everything here is expressed in DIPs at the boundary and
// in device pixels inside, which is the only way the DPI matrix stays honest - the
// layout contract (C-05) is defined in device pixels, the render backend (C-04)
// takes DIPs, and the conversion has to happen exactly once.
#pragma once

#include "core/config.h"
#include "dock/drag_controller.h"
#include "dock/drop_target.h"
#include "dock/layout.h"
#include "platform/win_common.h"
#include "render/backend.h"
#include "render/material.h"
#include "shell/app_entry.h"
#include "shell/app_resolver.h"
#include "shell/icon_cache.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dockforge::dock {

// Breathing room between the icon row and the plate edge, in DIP.
//
// Deliberately not a config key: it is a design constant, not a preference, and
// adding it to C-01 would create a field nobody can meaningfully tune while making
// every theme override longer.
inline constexpr float kPlatePaddingDip = 10.0f;

// Diameter of a running indicator, in DIP.
//
// The indicator lives inside the plate's existing bottom padding rather than in a
// band of its own. Adding a band would grow the plate and the window by 7 DIP, which
// moves every dock on screen and invalidates every measurement tool that derives
// scale from the window height. The padding is 10 DIP, so a 4 DIP dot centred in it
// leaves a 3 DIP gap under the icons - the proportion macOS uses.
inline constexpr float kIndicatorDotDip = 4.0f;

// Indicator opacity by state. The foreground dot is nearly opaque and the background
// one is deliberately faint: at 48 DIP the dots are small enough that equal weight
// would read as a row of beads rather than as one active app.
inline constexpr float kIndicatorOpacityForeground = 0.95f;
inline constexpr float kIndicatorOpacityRunning = 0.45f;

// ---- message badge (T-065, C-22 message_badge) ------------------------------
// The red dot is 28% of the icon's width and never smaller than 10 DIP (10px at
// 100% DPI); the number is 22% and never smaller than 9pt (= 12 DIP). Both
// minima are contract text, not taste - below them Windows' own toast badge
// becomes unreadable next to a 32px icon.
inline constexpr float kBadgeDiameterFactor = 0.28f;
inline constexpr float kBadgeMinDiameterDip = 10.0f;
inline constexpr float kBadgeFontFactor = 0.22f;
inline constexpr float kBadgeMinFontDip = 12.0f;  // 9pt at 96 DPI

// Fallback red when the system accent colour cannot be read. Close to the
// macOS badge red so the fallback still reads as "unread", not as an error.
inline constexpr render::ColorF kBadgeFallbackColor{0.91f, 0.27f, 0.24f, 1.0f};

// "12" for 1..9, "9+" above, empty at 0 and below (C-22: nothing is drawn).
std::wstring BadgeTextFor(int count);

// Badge geometry for one painted slot, in window DIP (C-22: derived from the
// same LayoutSummary the icon pass used - never a second coordinate source).
// The dot is centred on the slot's top-right corner, pulled slightly inward so
// the ring stays inside the window region at every DPI.
struct BadgeGeometry {
    render::RectF dot{};
    float font_dip = 0.0f;
};
BadgeGeometry ComputeBadgeGeometry(const render::RectF& slot);

// A tile's relationship to a live window, refreshed by the tracker (T-012).
//
// Three states rather than a bool because "running" and "frontmost" are answered by
// different sources at different times: existence comes from the window list, which
// changes when apps open and close; foreground comes from GetForegroundWindow,
// which changes on every alt-tab. Collapsing them into one flag would make the
// highlight lag behind the foreground change it is supposed to report.
enum class TileRunState {
    Idle,        // no window
    Running,     // has a window, not in front
    Foreground,  // has a window and it is the foreground window
};

const char* ToString(TileRunState state);

// One dock slot. The icon is kept by value so painting never touches the shell or
// the cache: a frame that blocks on GetImage is a 60-190ms stall (evidence/T-006).
struct DockTile {
    shell::AppEntry app;
    shell::IconBitmap icon;
    // Stable identity for the render backend's GPU bitmap cache. Derived from the
    // icon source rather than the tile index so reordering the dock does not
    // invalidate every texture.
    std::uint64_t icon_key = 0;
    // Written by RunningTracker::ApplyToTiles, read by the painter. Lives on the
    // tile rather than in a parallel array so the two cannot fall out of step when
    // the tile list changes length.
    TileRunState run_state = TileRunState::Idle;
    // Unread-message badge (T-065, C-22 message_badge). Written from the
    // MessageCounterHost's last reported counts by the main loop, read by the
    // painter. 0 means "no badge"; the painter draws nothing for it, so a tile
    // whose app has no adapter needs no special case.
    int message_count = 0;
};

// FNV-1a over the UTF-16 source string. Only needs to be collision-resistant
// within one dock's tile list, which is at most a few dozen entries.
std::uint64_t IconKeyFor(const std::wstring& source);

// Deterministic tile list: the AppsFolder scan is already sorted by display name,
// so taking the first `max_tiles` keeps the dock stable across runs instead of
// reshuffling whenever enumeration order changes.
std::vector<DockTile> BuildTiles(const std::vector<shell::AppEntry>& apps, shell::IconCache& icons,
                                 int max_tiles, int icon_px);

struct DockGeometry {
    LayoutParams params{};  // device pixels
    float scale = 1.0f;
    int tile_count = 0;
    float content_width_dip = 0.0f;  // idle icon row
    float plate_width_dip = 0.0f;    // idle row + padding on both sides
    float plate_height_dip = 0.0f;   // constant; does not grow on hover
    // Tallest an icon can get, plus padding. The window is always this tall so
    // magnification never resizes it: a resize would move the window under the
    // cursor (the row is centred) and re-arm the capture region on every hover.
    float window_width_dip = 0.0f;
    float window_height_dip = 0.0f;
    // Where the icon row's bottom edge sits, measured from the window's top. Icons
    // are bottom-aligned here and the indicator band occupies the space below it.
    float baseline_dip = 0.0f;
};

// Pure function of config + tile count + scale, so the window size can be asserted
// in a test without creating a window.
//
// Costs up to 256 layout evaluations (it sweeps the pointer to find the widest row
// the magnification can produce), so call it on config/DPI/tile-count changes only -
// never inside the paint path (C-05a rule 3).
DockGeometry ComputeGeometry(const core::Config& config, int tile_count, float scale);

// Slot centres of the UNMAGNIFIED row for a geometry, in window DIP.
//
// Free function rather than a DockPainter method because the OLE drop target needs the
// rest grid before the first paint has happened: the painter caches the geometry it
// last laid out, and reading that cache before any layout would answer with zeros.
std::vector<float> IdleCentersDip(const DockGeometry& geometry, float scale);

// Pointer state in dock-local DIPs.
//
// `intensity` ramps 0..1 on enter and back on leave rather than snapping: the peak
// scale is `magnification` a pixel inside the plate, so entering without a ramp
// pops the whole row from 1.0 to ~1.5 in one frame.
struct DockHover {
    bool active = false;
    float mouse_x_dip = 0.0f;
    float intensity = 0.0f;
};

// How fast `DockHover::intensity` follows `active`, in units per second. 8/s means
// ~125ms to full magnification, which is inside the "feels instant, not jumpy"
// band and short enough that a quick pass over the dock still registers.
inline constexpr float kHoverRampPerSecond = 8.0f;

// Advances `intensity` toward its target. Returns true when the value changed, so
// the caller knows whether another frame is needed (false means the dock is at rest
// and the frame loop can go back to sleep).
bool AdvanceHover(DockHover& hover, double delta_ms);

struct DockPaintInput {
    const core::Config* config = nullptr;
    const render::PlateStyle* style = nullptr;
    const std::vector<DockTile>* tiles = nullptr;
    // Precomputed by the caller via ComputeGeometry. Required: recomputing it here
    // would sweep 256 layouts on every hover frame.
    DockGeometry geometry{};
    const std::uint8_t* backdrop = nullptr;
    int backdrop_w = 0;
    int backdrop_h = 0;
    float dpi = 96.0f;
    DockHover hover{};
    // Live reorder gesture, or nullptr. The painter reads it but never writes: the
    // controller owns the gesture, and two owners would let the ghost and the
    // insertion slot disagree.
    const DragState* drag = nullptr;
    // In-flight OLE drag from another application, or nullptr. Same rule as `drag`:
    // the drop target owns it, the painter only reads.
    const DropPointer* drop = nullptr;
};

// Opacity of the lifted tile's ghost while dragging.
//
// Not 1.0: the ghost has to read as "held", not as "the icon grew and stopped
// tracking the row". Not far below either - at 0.7 the artwork washes out against a
// bright backdrop and the user loses track of what they are moving.
inline constexpr float kDragGhostOpacity = 0.82f;

// Thickness of the insertion line, in DIP.
inline constexpr float kDropLineWidthDip = 3.0f;
// Height of the insertion line as a fraction of the icon size. A full-height line
// competes with the icons; a short one reads as a caret marking a text position.
inline constexpr float kDropLineHeightFactor = 0.62f;

// Colour of the insertion caret while an external drag is over the dock. White when
// the payload would be accepted, warm red when it would not: the cursor already shows
// a "no drop" badge, but that badge belongs to the source application, and a dock that
// only relied on it would look willing right up until the release did nothing.
inline constexpr render::ColorF kDropLineAcceptColor{0.92f, 0.94f, 0.98f, 0.90f};
inline constexpr render::ColorF kDropLineRejectColor{0.95f, 0.42f, 0.38f, 0.90f};

// Width of the caret shown for an external drag, in DIP. Wider than the internal
// reorder caret because it also has to be visible against a backdrop the dock does not
// control.
inline constexpr float kDropCaretWidthDip = 4.0f;

// Owns the per-frame scratch layout so painting does not allocate. A Dock is
// repainted on every hover frame, and C-05a rule 3 forbids heap traffic there.
class DockPainter {
  public:
    bool Paint(ID2D1DeviceContext& dc, render::IRenderBackend& backend,
               render::PlateRenderer& plate, const DockPaintInput& input);

    // Runs the layout for the given hover without painting and returns the summary.
    //
    // Exists so the clickable region and the drawn row are derived from ONE
    // computation. Reading the previous frame's summary instead would make the
    // region lag the paint by a frame, and because SetWindowRgn clips composition
    // content, a lagging region clips the very icons it was meant to expose -
    // measured: the magnified row was cut off exactly at the plate's top edge.
    const LayoutSummary& UpdateLayout(const DockGeometry& geometry, const DockHover& hover,
                                      int tile_count);

    const LayoutSummary& LastLayout() const { return last_; }

    // Which tile covers `x_dip`, or -1 for a miss.
    //
    // Reads the layout produced by the last UpdateLayout, so the click target is the
    // icon the user actually sees rather than the idle slot it would occupy. Hit
    // testing the idle geometry would make the lifted icon under the cursor
    // unclickable and its pushed-aside neighbour clickable - the worst possible
    // trade, because the icon under the pointer is the one being aimed at.
    //
    // Vertical bounds are not checked: SetWindowRgn already trims the window to the
    // plate plus the magnified row, so a click that reaches this handler is inside
    // the visible dock.
    int HitTest(float x_dip, float scale) const;

    // Slot centres in window DIP, in tile order, from the last UpdateLayout.
    //
    // The drag controller needs them to decide the insertion slot, and they must be
    // the same numbers Paint used - recomputing them from the geometry in a second
    // place is how the ghost and the insertion line drift apart.
    std::vector<float> IconCentersDip(float scale) const;

    // Slot centres of the UNMAGNIFIED row, in window DIP.
    //
    // This, not IconCentersDip, is what the insertion slot is computed from. The
    // magnified row is a function of the pointer position, so deciding the slot from
    // it closes a feedback loop: moving right magnifies the icons on the right, which
    // pushes their centres further right, which changes which slot the pointer is
    // "past". Measured consequence: dragging the first tile onto the visual centre of
    // the fifth reported slot 3, and the answer moved when the drag was replayed from a
    // different starting slot. The idle grid is fixed, so the same gesture always lands
    // in the same place.
    std::vector<float> IdleCentersDip(float scale) const;

    // Where the row's bottom edge sits, in window DIP. Exposed so the drag ghost can
    // be aligned to the same baseline as the icons without duplicating the geometry
    // lookup.
    float BaselineDip() const { return baseline_dip_; }

  private:
    std::vector<IconLayout> scratch_;
    LayoutSummary last_{};
    // Where the row starts inside the window, in DIP. Stored by UpdateLayout because
    // HitTest needs the same origin Paint used, and recomputing it from the geometry
    // in two places is how the two drift apart.
    float row_origin_dip_ = 0.0f;
    // Baseline in window DIP, cached by UpdateLayout for the same reason.
    float baseline_dip_ = 0.0f;
    // Last geometry seen by UpdateLayout. Cached so IdleCentersDip can rebuild the
    // rest-state grid without another layout pass and without trusting that the last
    // paint happened to be a resting one.
    DockGeometry geometry_{};
};

}  // namespace dockforge::dock
