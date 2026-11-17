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
// The trailing status group is the right region (T-307, `finderbar.right`): its
// layout function lives in this file too, so both halves of the bar stay pure
// arithmetic and the width the right region occupies flows into the left group's
// usable-space computation through BarLayoutParams::status_width_dip.
#pragma once

#include "platform/win_common.h"
#include "render/backend.h"

#include <cstddef>
#include <optional>
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

// ---------------------------------------------------------------------------
// Right group (T-307, CONTRACTS.md C-31 `finderbar.right`).
//
// The right region is a second row of DIP-space boxes laid out from the RIGHT edge
// inwards. C-31 enumerates eight widget types:
//
//   tray_mirror, control_center, notification_center, ime, battery, network, clock,
//   weather_brief
//
// which is exactly the order the enum below keeps: C-31 declares the type set, and
// (per the T-307 spec) two entries that claim the same `order` are ordered by that
// declaration sequence, so the rendered result is deterministic no matter how the
// user wrote their file.
//
// ORDER SEMANTICS (frozen wording, also recorded in DECISIONS.md ADR-0053):
//
//   `order` is a position counted FROM THE RIGHT EDGE. 0 = rightmost. The default
//   table is [{type:clock,order:0}] - the clock sits hard against the right edge,
//   which is where macOS puts its clock.
//
//   Insertion convention for the later tasks (T-303..T-306), so every task writes
//   the same defaults without re-deriving them:
//
//     control_center       = 1
//     notification_center  = 2
//     tray_mirror          = 3
//     network              = 4
//     battery              = 5
//     ime                  = 6
//
//   i.e. the macOS reading order, left to right: clock <- control center <-
//   notifications <- tray mirror <- status icons. weather_brief stays OUT of the
//   default table; a user who wants it writes [{type:"weather_brief",order:1}] and
//   it appears to the LEFT of the clock. A type claiming an order outside 0..6
//   still lays out - the convention pins only the defaults, not the range.
//
//   Same-order conflicts: the entry whose type comes first in C-31's declaration
//   sequence (lower enum value) is placed further RIGHT. Ties therefore resolve as
//   if the file had listed them in declaration order - deterministic, and stable
//   under std::stable_sort semantics for equal (order, type) pairs.
//
// Not every type is renderable yet. Registration is explicit so a type whose task
// has not landed is SKIPPED, never drawn as an empty placeholder slot: a gap for a
// widget that does not exist is exactly the "reserved for nothing" damage T-301's
// status area avoided. Later tasks flip their type to registered here and nowhere
// else.
// ---------------------------------------------------------------------------

enum class RightType {
    TrayMirror = 0,        // T-303
    ControlCenter,         // T-304
    NotificationCenter,    // T-305
    Ime,                   // T-306
    Battery,               // T-306
    Network,               // T-306
    Clock,                 // T-307
    WeatherBrief,          // T-307
};

// The C-31 spelling of each type ("clock", "weather_brief", ...). Declaration order,
// so a caller can iterate [0, Count) to enumerate in C-31 order.
constexpr int kRightTypeCount = 8;
const wchar_t* RightTypeName(int type_index);
std::optional<RightType> RightTypeFromName(const wchar_t* name);

// True when a renderer exists for this type in the current build. T-307 registers
// exactly Clock and WeatherBrief; the other six flip when their tasks land.
bool RightTypeRegistered(RightType type);

// Side of the glyph box a right entry carries, 16 DIP: the same raster family as
// kSystemMenuGlyphDip (14 DIP) - one notch larger because status icons sit next to
// 13 DIP text, and 14 would read small against it. Beautiful's top bar uses 16x16
// status images for the same reason (TopBarWindow.xaml.cs NewStatusImage).
// Declared above the entry structs because RightItem's default member initializer
// references it.
constexpr float kRightIconBoxDip = 16.0f;

// One right-region entry, already rendered into its display form. The layout is a
// pure function over these: a renderer (finder_bar.cpp) produces the text/icon flags
// from live state, the layout only positions. An entry with neither icon nor text
// still gets a box (an icon-only entry carries its glyph in the painter); a caller
// that wants a type skipped filters it out before calling, which is what
// RightTypeRegistered is for.
struct RightItem {
    RightType type = RightType::Clock;
    int order = 0;          // 0 = rightmost (see the block comment above)
    std::wstring text;      // display text drawn at 13 DIP; empty = no text
    bool has_icon = false;  // draw a glyph box (kRightIconBoxDip tall; width below)
    // Width of the glyph box when has_icon. The default is the 16 DIP square every
    // status icon shares; T-306's battery needs the wider macOS frame (~25 DIP,
    // measured off the round10 top bar) and sets this. The height stays
    // kRightIconBoxDip and the painter centres its glyph inside.
    float icon_width_dip = kRightIconBoxDip;
    // The item's one alarm flag (T-306 battery <20%). The painter renders the text
    // in the frozen HIG systemRed when set; the three-tier grade machinery stays
    // the centre zone's because the spec pins no orange band for the battery.
    bool alert = false;
};

// One laid-out right-region box. Like BarBox, the box spans the full bar height so
// the whole row is clickable; `icon` is the inner glyph rect, valid when has_icon,
// vertically centred, `icon_width_dip` wide.
struct RightBox {
    render::RectF rect{};
    RightType type = RightType::Clock;
    std::wstring text;
    bool has_icon = false;
    bool alert = false;
    render::RectF icon{};
};

struct RightGroupSummary {
    // Width the right group occupies from the right edge inwards, NOT counting
    // kTrailingInsetDip (LayoutBar subtracts that itself). Feed this straight into
    // BarLayoutParams::status_width_dip so the left group can never run under it.
    float total_width_dip = 0.0f;
    std::size_t placed = 0;
    // Entries dropped because their width did not fit between the left group and the
    // leading edge. Reported, not hidden - same honesty rule as BarLayoutSummary.
    std::size_t dropped = 0;
};

// Lays out the right group from the right edge inwards and reports its total width.
//
// `items` are sorted by (order ascending, type ascending) with a stable sort, placed
// from the right edge (width_dip - kTrailingInsetDip) leftwards with
// kRightItemGapDip between boxes, and returned in left-to-right order so a paint loop
// and the hit test read naturally. An entry wider than the free space left of the
// left group is dropped and counted in `dropped` rather than drawn overlapping.
//
// Writes at most `capacity` boxes into `out` and returns how many were written.
std::size_t LayoutRightGroup(const RightItem* items, std::size_t count, float width_dip,
                             float height_dip, MeasureFn measure, RightBox* out,
                             std::size_t capacity, RightGroupSummary* summary);

// Convenience overload for callers that want a vector.
std::vector<RightBox> LayoutRightGroup(const std::vector<RightItem>& items, float width_dip,
                                       float height_dip, MeasureFn measure,
                                       RightGroupSummary* summary);

// Index of the right-region box containing the point, or kNoMenu. Same half-open
// scan as BoxIndexAt so no point ever matches two boxes.
std::size_t RightBoxIndexAt(const RightBox* boxes, std::size_t count, float x_dip, float y_dip);

// Gap between adjacent right-region boxes. macOS spaces its status items at roughly
// one icon width; 12 DIP matches kSystemMenuGapDip, keeping one rhythm across the
// bar instead of two.
constexpr float kRightItemGapDip = 12.0f;

// Side padding of a right-region box around its content. Text-only entries (the
// clock) and icon entries share it, so a hover highlight has equal breathing room
// on both shapes.
constexpr float kRightContentPaddingDip = 6.0f;

// Gap between a glyph box and its text inside one entry (weather_brief: glyph +
// temperature).
constexpr float kRightIconTextGapDip = 4.0f;

}  // namespace dockforge::finderbar
