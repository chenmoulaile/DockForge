// DockForge - Launchpad search bar and folder presentation (CONTRACTS.md C-18).
//
// The search field and the folder strip are pure geometry, like the grid, so both
// can be tested without a window. The rules that matter:
//
//   * the search bar occupies the top band the grid already reserves
//     (kLaunchpadTopBandDip), so adding one does not move the grid;
//   * the folder strip sits under the search bar and only exists when the user
//     actually has folders, so the grid is not pushed down for an empty feature;
//   * both report their rects in the same DIP space the grid uses, so the hit
//     test can walk them in one pass instead of the caller guessing.
#pragma once

#include <string>
#include <vector>

#include "dock/launchpad_layout.h"
#include "render/backend.h"

namespace dockforge::dock {

// The search bar lives in the top band the layout already reserves (kLaunchpadTopBandDip
// in launchpad_layout.h), which is why adding it moves nothing that already existed.

// Search bar geometry, DIP.
inline constexpr float kLaunchpadSearchHeightDip = 40.0f;
inline constexpr float kLaunchpadSearchWidthDip = 480.0f;
inline constexpr float kLaunchpadSearchRadiusDip = 20.0f;
inline constexpr float kLaunchpadSearchInsetDip = 12.0f;

// Search text metrics.
inline constexpr float kLaunchpadSearchFontDip = 15.0f;

// Folder strip geometry, DIP.
inline constexpr float kLaunchpadFolderChipHeightDip = 34.0f;
inline constexpr float kLaunchpadFolderChipGapDip = 10.0f;
inline constexpr float kLaunchpadFolderChipRadiusDip = 17.0f;
inline constexpr float kLaunchpadFolderChipPadDip = 16.0f;
inline constexpr float kLaunchpadFolderChipFontDip = 13.0f;
// Fixed chip width, not text-driven. A chip that resized on every rename would
// make the strip reflow under the pointer and steal the click the user was
// aiming at the next chip. The label is ellipsized by the painter when it does
// not fit.
inline constexpr float kLaunchpadFolderChipWidthDip = 168.0f;
inline constexpr float kLaunchpadFolderStripTopDip = kLaunchpadTopBandDip;
// How many folder names the strip can hold before it runs out of width. Used by
// the settings UI to warn before the user creates a folder they cannot see.
// Defined below, next to the layout function it shares the walk with.
int LaunchpadFolderChipLimit(float width_dip);

// The caret blink in the search field, milliseconds.
inline constexpr float kLaunchpadCaretBlinkMs = 530.0f;

// The search bar rect, horizontally centred in the viewport. The width is
// clamped to the viewport so a narrow display still gets a usable field rather
// than one that runs off both edges.
render::RectF LaunchpadSearchRect(float width_dip, float height_dip);

// Hit test for the search bar alone. True when the point is inside the field's
// rect, which is what decides whether a click focuses it instead of the grid.
bool LaunchpadSearchHit(float x_dip, float y_dip, float width_dip, float height_dip);

// The caret rect for a given character offset, so the caller can draw it without
// re-measuring the string. Returns a zero-width rect when the offset is out of
// range, which the painter treats as "no caret".
render::RectF LaunchpadSearchCaretRect(const std::wstring& text, int caret_index,
                                       const render::RectF& field);

// The caret rect for the folder rename field. Same fixed advance rule as the search
// caret, but anchored to the chip being renamed so the caller does not have to
// re-derive the chip rect it already holds.
render::RectF LaunchpadFolderRenameCaretRect(const std::wstring& text, int caret_index,
                                             const render::RectF& chip);

// The folder chip rects, one per folder, laid out left to right from the strip
// origin. `count` must be the number of folders; the rects vector is resized.
//
// Chips are clipped to the viewport: a user with 64 folders gets chips until the
// right edge, and the rest are simply not drawn. Scrolling a chip strip is a
// future feature; silently dropping chips past the edge is not a crash.
void LaunchpadFolderChipRects(float width_dip, int count, std::vector<render::RectF>& rects);

// Which chip the point is over, or -1. Walks the rects so the last (possibly
// clipped) chip is handled the same as the others.
int LaunchpadFolderChipHit(float x_dip, float y_dip, const std::vector<render::RectF>& rects);

}  // namespace dockforge::dock
