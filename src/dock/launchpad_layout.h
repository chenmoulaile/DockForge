// DockForge - Launchpad layout math (CONTRACTS.md C-18).
//
// Pure arithmetic, no Windows types beyond the shared render::RectF, so the grid rules
// can be tested without creating a window. The paint path and the hit test both call
// these functions rather than re-deriving positions, which is the same rule C-05 sets
// for the Dock row: one source for geometry.
#pragma once

#include <vector>

#include "render/backend.h"

namespace dockforge::dock {

// C-18 page geometry.
inline constexpr int kLaunchpadDefaultPageSize = 35;  // 5 x 7
inline constexpr int kLaunchpadMinColumns = 3;
inline constexpr int kLaunchpadMaxColumns = 12;

// Cell composition. The label sits under the icon and belongs to the cell's click
// target, so the cell is taller than icon + label + gap.
inline constexpr float kLaunchpadLabelHeightDip = 18.0f;
inline constexpr float kLaunchpadLabelGapDip = 6.0f;
inline constexpr float kLaunchpadColumnGapDip = 26.0f;
inline constexpr float kLaunchpadRowGapDip = 26.0f;

// Hover response. 1.10x is enough to read as "this one" on a full-screen grid without
// the neighbouring cells appearing to move; the Dock uses 1.8x because it magnifies a
// row the pointer travels along.
inline constexpr float kLaunchpadHoverScale = 1.10f;

// Open/close animation. Short on purpose: a launcher is a transit surface, and anything
// past ~200ms reads as latency rather than polish.
inline constexpr float kLaunchpadOpenMs = 180.0f;
inline constexpr float kLaunchpadCloseMs = 140.0f;
inline constexpr float kLaunchpadOpenScaleFrom = 0.94f;

// Page dots.
inline constexpr float kLaunchpadDotDip = 8.0f;
inline constexpr float kLaunchpadDotGapDip = 10.0f;
inline constexpr float kLaunchpadDotBottomMarginDip = 24.0f;

struct LaunchpadGridInput {
    float width_dip = 0.0f;
    float height_dip = 0.0f;
    int icon_size = 96;   // C-18 launchpad.icon_size, DIP
    int columns = 0;      // C-18 launchpad.columns, 0 = adaptive
    int page_size = kLaunchpadDefaultPageSize;
};

struct LaunchpadGrid {
    int columns = 0;
    int rows = 0;
    int per_page = 0;
    float cell_width_dip = 0.0f;
    float cell_height_dip = 0.0f;
    float origin_x_dip = 0.0f;
    float origin_y_dip = 0.0f;
    float icon_size_dip = 0.0f;
    float label_height_dip = 0.0f;
};

// Resolves columns, rows, and the cell geometry for one display. Never returns a grid
// with columns == 0 or rows == 0, so callers can divide by them on the first frame.
LaunchpadGrid ComputeLaunchpadGrid(const LaunchpadGridInput& input);

int LaunchpadPageCount(int item_count, const LaunchpadGrid& grid);
int LaunchpadPageItemCount(int item_count, const LaunchpadGrid& grid, int page);
int ClampLaunchpadPage(int page, int item_count, const LaunchpadGrid& grid);

// Cell rect for an index on the current page (0-based within the page).
render::RectF LaunchpadCellRect(const LaunchpadGrid& grid, int index_on_page);
render::RectF LaunchpadIconRect(const LaunchpadGrid& grid, int index_on_page);
render::RectF LaunchpadLabelRect(const LaunchpadGrid& grid, int index_on_page);

// Returns the index on the page under the point, or -1. Walks the cells rather than
// dividing by cell size: the label band belongs to the cell, and the last row can be
// short, so a division would report hits in empty space.
int LaunchpadHitTest(const LaunchpadGrid& grid, int items_on_page, float x_dip, float y_dip);

// Icon edge length with the hover magnification applied, clamped so a hovered icon
// cannot grow into its neighbours' gutter.
float LaunchpadIconEdgeFor(const LaunchpadGrid& grid, float hover);

float LaunchpadDotCenterYDip(const LaunchpadGrid& grid, float height_dip);
render::RectF LaunchpadDotRect(const LaunchpadGrid& grid, float height_dip, int page_index,
                               int page_count);

// C-18: the indicator only exists when there is more than one page.
inline bool LaunchpadShowsPageDots(int page_count) { return page_count > 1; }

// Animation curves. Progress runs 0..1 for open and 1..0 for close; both helpers take
// the raw progress so the paint path stays a pure function of it.
float LaunchpadAnimationScale(float progress);
float LaunchpadAnimationFade(float progress);

// Advances progress by delta_ms over duration_ms, clamped to 0..1. A negative delta
// rewinds, which is how the close animation reuses the same curve.
float AdvanceLaunchpadProgress(float progress, float delta_ms, float duration_ms);

}  // namespace dockforge::dock
