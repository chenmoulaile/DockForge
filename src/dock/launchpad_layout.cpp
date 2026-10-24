#include "dock/launchpad_layout.h"

#include <algorithm>
#include <cmath>

namespace dockforge::dock {
namespace {

// Horizontal room kept free on both sides so the outermost icon never touches the
// display edge. Not adaptive: the grid already adapts its column count, and letting
// both vary makes a two-column difference between two similar screens look like a
// bug rather than a layout decision.
constexpr float kSideMarginDip = 64.0f;

// Bands above and below the grid. The top band is where a title/search field will
// live (T-042); reserving it now keeps the grid from moving when that lands. The
// bottom band holds the page dots.
constexpr float kTopBandDip = 56.0f;
constexpr float kBottomBandDip = 48.0f;

// A gutter is guaranteed even when the icon was shrunk to fit its cell, so two
// adjacent icons never read as one wide shape.
constexpr float kMinIconGutterDip = 8.0f;

// Floor for the shrink-to-fit path. Below this an icon is not recognisable, and an
// overflowing grid is the lesser evil - it is at least still clickable.
constexpr float kMinIconEdgeDip = 16.0f;

float EaseOutCubic(float t) {
    const float inv = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - inv * inv * inv;
}

}  // namespace

LaunchpadGrid ComputeLaunchpadGrid(const LaunchpadGridInput& input) {
    LaunchpadGrid grid;
    const int page_size = std::max(1, input.page_size);
    const float requested_icon = static_cast<float>(std::max(1, input.icon_size));

    grid.per_page = page_size;
    grid.icon_size_dip = requested_icon;
    grid.label_height_dip = kLaunchpadLabelHeightDip;

    if (input.width_dip <= 0.0f || input.height_dip <= 0.0f) {
        // Degenerate frame (a window that has not been sized yet). Return a legal
        // grid rather than NaNs: the first paint must not divide by zero.
        grid.columns = std::clamp(input.columns > 0 ? input.columns : 5, kLaunchpadMinColumns,
                                  kLaunchpadMaxColumns);
        grid.rows = (page_size + grid.columns - 1) / grid.columns;
        return grid;
    }

    const float available_w = std::max(1.0f, input.width_dip - kSideMarginDip * 2.0f);

    // C-18: an explicit column count wins; otherwise derive it from how many
    // icon-sized cells fit, then clamp. The floor of 3 keeps the grid from turning
    // into a single column on a narrow window; the ceiling of 12 stops a very wide
    // display from producing a strip of unreadably small cells.
    int columns = input.columns;
    if (columns <= 0) {
        const float pitch = requested_icon + kLaunchpadColumnGapDip;
        columns = static_cast<int>(std::floor(available_w / pitch));
    }
    columns = std::clamp(columns, kLaunchpadMinColumns, kLaunchpadMaxColumns);

    grid.columns = columns;

    // `available_w / columns` is the pitch, not the cell width: the column gap comes out
    // of it. Treating the pitch as the cell would pack the cells edge to edge, and the
    // 26 DIP of air between two icons would then belong to one of their click targets -
    // a click in what looks like empty space would launch an app.
    const float pitch_x = available_w / static_cast<float>(columns);
    grid.cell_width_dip = std::max(kMinIconEdgeDip, pitch_x - kLaunchpadColumnGapDip);

    // Shrink the icon if the cell is narrower than requested plus the minimum
    // gutter. This is what makes an explicit `columns = 12` on a 1280-wide display
    // still legible instead of an overlapping smear.
    float icon_edge = std::min(requested_icon, grid.cell_width_dip - kMinIconGutterDip);
    icon_edge = std::max(icon_edge, kMinIconEdgeDip);

    grid.rows = (page_size + columns - 1) / columns;
    grid.origin_x_dip = kSideMarginDip;

    const float available_h = input.height_dip - kTopBandDip - kBottomBandDip;
    float cell_height = icon_edge + kLaunchpadLabelGapDip + kLaunchpadLabelHeightDip;
    float grid_height =
        static_cast<float>(grid.rows) * cell_height + static_cast<float>(grid.rows - 1) * kLaunchpadRowGapDip;

    // Shrink to fit vertically as well. `rows` is fixed by the contract, so the only
    // free variable left is the icon edge.
    if (grid_height > available_h && grid.rows > 0) {
        const float max_cell_height =
            (available_h - static_cast<float>(grid.rows - 1) * kLaunchpadRowGapDip) /
            static_cast<float>(grid.rows);
        const float max_icon =
            max_cell_height - kLaunchpadLabelGapDip - kLaunchpadLabelHeightDip;
        if (max_icon < icon_edge) {
            icon_edge = std::max(max_icon, kMinIconEdgeDip);
            cell_height = icon_edge + kLaunchpadLabelGapDip + kLaunchpadLabelHeightDip;
            grid_height = static_cast<float>(grid.rows) * cell_height +
                          static_cast<float>(grid.rows - 1) * kLaunchpadRowGapDip;
        }
    }

    grid.icon_size_dip = icon_edge;
    grid.cell_height_dip = cell_height;

    // Centre the block in the band between the reserved top and bottom areas. A
    // negative remainder (the grid is taller than the display) pins it to the top
    // band so the first row stays visible.
    const float slack = available_h - grid_height;
    grid.origin_y_dip = kTopBandDip + (slack > 0.0f ? slack * 0.5f : 0.0f);

    return grid;
}

int LaunchpadPageCount(int item_count, const LaunchpadGrid& grid) {
    const int per_page = std::max(1, grid.per_page);
    if (item_count <= 0) return 1;
    return (item_count + per_page - 1) / per_page;
}

int LaunchpadPageItemCount(int item_count, const LaunchpadGrid& grid, int page_index) {
    const int per_page = std::max(1, grid.per_page);
    if (item_count <= 0 || page_index < 0) return 0;
    const int start = page_index * per_page;
    if (start >= item_count) return 0;
    return std::min(per_page, item_count - start);
}

int ClampLaunchpadPage(int page_index, int item_count, const LaunchpadGrid& grid) {
    const int last = LaunchpadPageCount(item_count, grid) - 1;
    return std::clamp(page_index, 0, std::max(0, last));
}

render::RectF LaunchpadCellRect(const LaunchpadGrid& grid, int index_on_page) {
    if (index_on_page < 0 || grid.columns <= 0) return render::RectF{};
    const int col = index_on_page % grid.columns;
    const int row = index_on_page / grid.columns;
    // The pitch is cell + gap, matching ComputeLaunchpadGrid: the cell width already had
    // the gap subtracted from it, so adding it back here reproduces the original spacing.
    const float pitch_x = grid.cell_width_dip + kLaunchpadColumnGapDip;
    const float left = grid.origin_x_dip + static_cast<float>(col) * pitch_x;
    const float top = grid.origin_y_dip +
                      static_cast<float>(row) * (grid.cell_height_dip + kLaunchpadRowGapDip);
    return render::RectF{left, top, left + grid.cell_width_dip, top + grid.cell_height_dip};
}

render::RectF LaunchpadIconRect(const LaunchpadGrid& grid, int index_on_page) {
    const render::RectF cell = LaunchpadCellRect(grid, index_on_page);
    const float edge = grid.icon_size_dip;
    const float left = cell.CenterX() - edge * 0.5f;
    return render::RectF{left, cell.top, left + edge, cell.top + edge};
}

render::RectF LaunchpadLabelRect(const LaunchpadGrid& grid, int index_on_page) {
    const render::RectF cell = LaunchpadCellRect(grid, index_on_page);
    const float top = cell.top + grid.icon_size_dip + kLaunchpadLabelGapDip;
    return render::RectF{cell.left, top, cell.right, top + grid.label_height_dip};
}

int LaunchpadHitTest(const LaunchpadGrid& grid, int items_on_page, float x_dip, float y_dip) {
    if (items_on_page <= 0) return -1;
    // Walks cells rather than dividing by the cell size: the label band is part of
    // the target, and a division-based test would need the same two branches anyway
    // once the last row is short.
    const int cells = std::min(items_on_page, grid.per_page);
    for (int i = 0; i < cells; ++i) {
        const render::RectF cell = LaunchpadCellRect(grid, i);
        if (x_dip >= cell.left && x_dip <= cell.right && y_dip >= cell.top &&
            y_dip <= cell.bottom) {
            return i;
        }
    }
    return -1;
}

float LaunchpadIconEdgeFor(const LaunchpadGrid& grid, float hover) {
    const float amount = std::clamp(hover, 0.0f, 1.0f);
    return grid.icon_size_dip * (1.0f + (kLaunchpadHoverScale - 1.0f) * amount);
}

float LaunchpadDotCenterYDip(const LaunchpadGrid& grid, float height_dip) {
    (void)grid;
    return height_dip - kLaunchpadDotBottomMarginDip;
}

render::RectF LaunchpadDotRect(const LaunchpadGrid& grid, float height_dip, int page_index,
                               int page_count) {
    if (page_count <= 1 || page_index < 0 || page_index >= page_count) {
        return render::RectF{};
    }
    const float total = static_cast<float>(page_count) * kLaunchpadDotDip +
                        static_cast<float>(page_count - 1) * kLaunchpadDotGapDip;
    const float left = (grid.origin_x_dip + (grid.cell_width_dip * grid.columns) * 0.5f) -
                       total * 0.5f + static_cast<float>(page_index) *
                                          (kLaunchpadDotDip + kLaunchpadDotGapDip);
    const float center_y = LaunchpadDotCenterYDip(grid, height_dip);
    return render::RectF{left, center_y - kLaunchpadDotDip * 0.5f, left + kLaunchpadDotDip,
                         center_y + kLaunchpadDotDip * 0.5f};
}

float LaunchpadAnimationScale(float progress) {
    const float eased = EaseOutCubic(progress);
    return kLaunchpadOpenScaleFrom + (1.0f - kLaunchpadOpenScaleFrom) * eased;
}

float LaunchpadAnimationFade(float progress) { return EaseOutCubic(progress); }

float AdvanceLaunchpadProgress(float progress, float delta_ms, float duration_ms) {
    if (duration_ms <= 0.0f) return progress < 0.5f ? 0.0f : 1.0f;
    const float step = delta_ms / duration_ms;
    return std::clamp(progress + step, 0.0f, 1.0f);
}

}  // namespace dockforge::dock
