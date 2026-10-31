// DockForge - Finder Bar layout (T-301, CONTRACTS.md C-31/C-32).
//
// Everything in this file is a pure function of numbers. That is deliberate and it
// is the only reason this module is testable: the Dock and Launchpad learned the
// same lesson, and the geometry helpers they expose (ComputeBadgeGeometry,
// LaunchpadIconEdgeFor, IdleCentersDip) are the only parts of those modules with
// unit tests, because a renderer that needs a window cannot be tested in CI.
//
// The bar is a single row of DIP-space boxes laid out left to right:
//
//   [apple] [AppName]  [File] [Edit] [View] .......... [status]
//   ^------------- left group, owned by this task ----------^
//
// The trailing status group belongs to T-302..T-307. This module only reports where
// it starts, so the centre can be added later without re-deriving the left side.
#pragma once

#include "platform/win_common.h"
#include "render/backend.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dockforge::finderbar {

// C-31: 28 DIP at 100%. Named so a caller reads the intent, not the number.
constexpr float kDefaultBarHeightDip = 28.0f;

// macOS insets the leading edge by 20 DIP. The system-menu glyph is 14 DIP and the
// pointer target is the full bar height, so the visible margin is what reads as
// breathing room.
constexpr float kLeadingInsetDip = 20.0f;

// Gap between the system-menu glyph box and the application name.
constexpr float kSystemMenuGapDip = 12.0f;

// macOS uses a wider gap after the application name than between menu titles. That
// asymmetry is what makes the name read as a group of its own rather than as the
// first menu.
constexpr float kNameMenuGapDip = 14.0f;
constexpr float kMenuTitleGapDip = 18.0f;

// Horizontal padding inside one menu title box.
constexpr float kMenuTitlePaddingDip = 8.0f;

// The system-menu glyph is drawn, not a font character: the Apple logo is not in any
// font we can ship and shipping the mark is a trademark problem (ASSETS.md). The box
// is square-ish and centred in the bar.
constexpr float kSystemMenuGlyphDip = 14.0f;

// Trailing inset before the reserved status area.
constexpr float kTrailingInsetDip = 12.0f;

// Extra padding on the overflow box, which carries a single ellipsis.
constexpr float kOverflowPaddingDip = 10.0f;

// macOS uses 13pt SF Pro at 100%. The backend takes DIP; 13 DIP keeps the same
// optical weight against a 28 DIP bar.
constexpr float kMenuFontDip = 13.0f;
constexpr float kAppNameFontDip = 13.0f;

// Sentinel for "this box does not open a specific menu".
constexpr std::size_t kNoMenu = static_cast<std::size_t>(-1);

// One laid-out box in the bar. `label` is copied in so the layout owns its text and
// a caller cannot dangle it by rebuilding the source vector.
struct BarBox {
    render::RectF rect{};
    std::wstring label;
    // Index into the caller's menu list, or kNoMenu for the system menu, the
    // application name and the overflow box.
    std::size_t menu_index = kNoMenu;
    bool is_system_menu = false;
    bool is_app_name = false;
    bool is_overflow = false;
};

// What the bar needs to lay itself out. A value type with no pointers, so the layout
// stays a pure function and a test can exercise a bar no config could produce.
struct BarLayoutParams {
    float width_dip = 0.0f;
    float height_dip = kDefaultBarHeightDip;
    bool show_system_menu = true;
    bool show_app_name = true;
    bool show_app_menu = true;
    int max_menu_items = 6;
    // Width reserved at the trailing edge for the status area. T-301 renders nothing
    // there; the value comes from the caller so T-302 can grow it without touching
    // this file.
    float status_width_dip = 0.0f;
    // The foreground application's display name. Empty means "no name box".
    std::wstring app_name;
};

struct BarLayoutSummary {
    // How many of the caller's menu titles were actually placed.
    std::size_t placed_menus = 0;
    // True when at least one title did not fit and an overflow box was added.
    bool overflowed = false;
    // Total width the left group occupies, including the leading inset.
    float left_group_width_dip = 0.0f;
    // Where the status area begins, in DIP from the bar's left edge.
    float status_start_dip = 0.0f;
    // True when the boxes ran past the usable right edge. Only reachable when the bar
    // is narrower than the leading inset plus one title; reported rather than hidden
    // so a caller can log it.
    bool clipped = false;
};

// Measures one label the way the renderer will draw it. The real backend needs a D2D
// device context, so the layout takes a measuring callback instead of reaching for
// one: production passes real text metrics, tests pass a deterministic fake, and both
// exercise the same arithmetic.
using MeasureFn = float (*)(const wchar_t* text, float size_dip);

// Deterministic measurer used by tests and by the fallback path when no real measurer
// is available. Roughly matches Segoe UI at 13 DIP: 0.421 em average advance for
// mixed-case Latin, which is what a menu title mostly is.
float ApproximateTextWidth(const wchar_t* text, float size_dip);

// Lays out the left group and reports where the status area starts.
//
// `menu_titles` are the foreground application's top-level menu titles, in order.
// They are measured, not assumed: a caller that passes a 400 DIP title gets an
// overflow box rather than a bar that runs under the status area.
//
// Writes at most `capacity` boxes into `out` and returns how many were written.
std::size_t LayoutBar(const BarLayoutParams& params, const std::vector<std::wstring>& menu_titles,
                      MeasureFn measure, BarBox* out, std::size_t capacity,
                      BarLayoutSummary* summary);

// Convenience overload for callers that want a vector.
std::vector<BarBox> LayoutBar(const BarLayoutParams& params,
                              const std::vector<std::wstring>& menu_titles, MeasureFn measure,
                              BarLayoutSummary* summary);

// Which menu the box at (x_dip, y_dip) opens, or kNoMenu when the point is on empty
// bar or on a box that opens nothing. The caller distinguishes "empty bar" from "the
// system menu" by looking up the box, so this returns the index rather than a bool.
std::size_t HitTestBar(const BarBox* boxes, std::size_t count, float x_dip, float y_dip);

// Index of the box containing the point, or kNoMenu. Same scan as HitTestBar but
// keeps the box identity, which is what a click handler needs.
std::size_t BoxIndexAt(const BarBox* boxes, std::size_t count, float x_dip, float y_dip);

// Where the auto-hide hairline sits when the bar is retracted, in DIP. A fully hidden
// bar is unreachable; macOS leaves a few pixels so the pointer can find it.
constexpr float kAutoHideHairlineDip = 4.0f;

// Duration of a full reveal, in milliseconds. 180ms is the macOS menu-bar reveal
// duration to within a frame; shorter reads as a snap, longer as a lag.
constexpr double kAutoHideDurationMs = 180.0;

// Progress of the auto-hide animation, 0 = retracted, 1 = fully shown. Separate from
// the tick loop so the curve is testable and a probe can drive it directly.
struct AutoHideState {
    float progress = 1.0f;
    bool shown = true;
};

// Advances the reveal animation.
//
// `pointer_in_trigger` is true when the pointer is inside the top trigger strip;
// `pointer_in_bar` is true when it is inside the bar's own visible rectangle. The bar
// stays open while the pointer is over either, which is what makes a menu usable:
// retracting under an open menu would move the target mid-click.
//
// `delta_ms` is clamped internally so a stalled frame cannot teleport the bar.
void AdvanceAutoHide(AutoHideState& state, bool pointer_in_trigger, bool pointer_in_bar,
                     double delta_ms);

// The y offset the bar is drawn at, in DIP, for the current progress. At progress 1
// the offset is 0 (fully visible); at 0 it is -(height - hairline), so the hairline
// stays on screen.
float AutoHideOffsetDip(const AutoHideState& state, float height_dip);

// True when the bar should accept clicks at all. A retracted bar takes none: its
// visible part is a hairline the user is aiming at, and eating that click is the
// difference between "the bar is there" and "the desktop is broken".
bool AutoHideInteractive(const AutoHideState& state);

}  // namespace dockforge::finderbar
