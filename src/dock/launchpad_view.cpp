#include "dock/launchpad_view.h"

#include "core/log.h"
#include "shell/app_resolver.h"

#include <algorithm>
#include <cmath>

namespace dockforge::dock {
namespace {

constexpr char kLogModule[] = "launchpad";

// Hover ramp. Faster than the dock's 8/s because a Launchpad is a transient surface:
// the pointer is usually already moving when the grid appears, and a slow ramp would
// mean the first icon the user aims at is still growing when they click.
constexpr float kLaunchpadHoverRampPerSecond = 14.0f;

// Scrim over the desktop behind the grid. The Launchpad covers the whole display, so
// without a scrim the icons sit directly on whatever was there and the labels lose
// contrast against a bright window.
constexpr render::ColorF kScrimColor{0.05f, 0.06f, 0.08f, 0.55f};

// The plate behind the grid itself. Slightly lighter than the scrim so the grid reads
// as a surface rather than as floating icons.
constexpr render::ColorF kGridPlateColor{0.10f, 0.11f, 0.14f, 0.72f};

constexpr render::ColorF kLabelColor{0.92f, 0.94f, 0.98f, 0.92f};
constexpr render::ColorF kSelectionColor{0.92f, 0.94f, 0.98f, 0.14f};
constexpr render::ColorF kPlaceholderRingColor{0.62f, 0.66f, 0.74f, 0.35f};
constexpr render::ColorF kDotColorActive{0.92f, 0.94f, 0.98f, 0.90f};
constexpr render::ColorF kDotColorIdle{0.92f, 0.94f, 0.98f, 0.32f};

constexpr float kLabelSizeDip = 11.0f;
constexpr float kGridPlateRadiusDip = 24.0f;
constexpr float kSelectionRadiusDip = 18.0f;
constexpr float kSelectionInsetDip = 4.0f;

}  // namespace

std::uint64_t LaunchpadIconKey(const std::string& identity) {
    // FNV-1a 64. Chosen over std::hash because the value must be identical across
    // runs and across standard-library versions: the backend's GPU bitmap cache is
    // keyed on it, and a hash that changed between builds would re-upload every
    // icon after an update for no reason.
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char c : identity) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<LaunchpadTile> BuildLaunchpadTiles(const std::vector<shell::LaunchpadItem>& items,
                                               shell::IconCache& icons, int max_tiles,
                                               int icon_px) {
    std::vector<LaunchpadTile> tiles;
    const int limit = std::min(static_cast<int>(items.size()), std::max(0, max_tiles));
    tiles.reserve(static_cast<std::size_t>(limit));

    for (int i = 0; i < limit; ++i) {
        const shell::LaunchpadItem& item = items[static_cast<std::size_t>(i)];
        LaunchpadTile tile;
        tile.app = item.app;
        tile.identity = item.identity;
        tile.icon_key = LaunchpadIconKey(item.identity);
        // A missing icon is not fatal: the grid draws a ring so the cell stays
        // clickable and the user can still launch the app by name. Dropping the tile
        // would make an app disappear because the shell refused one image request.
        if (!icons.Get(shell::IconSourceFor(item.app), icon_px, tile.icon)) {
            DF_DEBUG(kLogModule, "no icon for %s", item.identity.c_str());
        }
        tiles.push_back(std::move(tile));
    }
    return tiles;
}

bool AdvanceLaunchpadHover(LaunchpadHover& hover, int cells, double delta_ms) {
    if (cells < 0) cells = 0;
    const std::size_t size = static_cast<std::size_t>(cells);
    if (hover.amounts.size() != size) hover.amounts.resize(size, 0.0f);

    const float step = static_cast<float>(delta_ms) * kLaunchpadHoverRampPerSecond / 1000.0f;
    bool moved = false;
    for (int i = 0; i < cells; ++i) {
        const float target = (hover.active && hover.index_on_page == i) ? 1.0f : 0.0f;
        float& amount = hover.amounts[static_cast<std::size_t>(i)];
        if (amount == target) continue;
        if (amount < target) {
            amount = std::min(target, amount + step);
        } else {
            amount = std::max(target, amount - step);
        }
        moved = true;
    }
    return moved;
}

bool LaunchpadPainter::Paint(ID2D1DeviceContext& dc, render::IRenderBackend& backend,
                             const LaunchpadPaintInput& input) {
    if (!input.tiles) return false;
    (void)dc;  // the backend owns the context; kept in the signature for parity with DockPainter

    PaintBackdrop(backend, input);
    PaintCells(backend, input);
    PaintDots(backend, input);
    return true;
}

void LaunchpadPainter::PaintBackdrop(render::IRenderBackend& backend,
                                     const LaunchpadPaintInput& input) {
    const float fade = LaunchpadAnimationFade(input.progress);
    const float width = input.width_dip;
    const float height = input.height_dip;
    if (width <= 0.0f || height <= 0.0f) return;

    // Scrim first, full-bleed. Its alpha carries the fade so the desktop shows
    // through for the first frames instead of the grid appearing on a hard black cut.
    render::ColorF scrim = kScrimColor;
    scrim.a *= fade;
    backend.FillRoundedRect(render::RectF{0.0f, 0.0f, width, height}, 0.0f, scrim);

    // Grid plate: the rect that actually holds the cells, padded out so the outermost
    // labels are not flush against its edge.
    const float pad = kSelectionInsetDip * 4.0f;
    const float plate_left = std::max(0.0f, input.grid.origin_x_dip - pad);
    const float plate_right = std::min(width, input.grid.origin_x_dip +
                                                  input.grid.cell_width_dip *
                                                      static_cast<float>(input.grid.columns) +
                                          pad);
    const float plate_top = std::max(0.0f, input.grid.origin_y_dip - pad);
    const float plate_bottom =
        std::min(height, input.grid.origin_y_dip +
                             static_cast<float>(input.grid.rows) *
                                 (input.grid.cell_height_dip + kLaunchpadRowGapDip) -
                             kLaunchpadRowGapDip + pad);

    if (plate_right > plate_left && plate_bottom > plate_top) {
        render::ColorF plate = kGridPlateColor;
        plate.a *= fade;
        backend.FillRoundedRect(render::RectF{plate_left, plate_top, plate_right, plate_bottom},
                                kGridPlateRadiusDip, plate);
    }
}

void LaunchpadPainter::PaintCells(render::IRenderBackend& backend,
                                  const LaunchpadPaintInput& input) {
    const int cells = std::min(input.items_on_page, input.grid.per_page);
    if (cells <= 0) return;

    const float fade = LaunchpadAnimationFade(input.progress);
    const int base = input.page * input.grid.per_page;

    for (int i = 0; i < cells; ++i) {
        const int item_index = base + i;
        if (item_index < 0 || item_index >= static_cast<int>(input.tiles->size())) continue;
        const LaunchpadTile& tile = (*input.tiles)[static_cast<std::size_t>(item_index)];

        float hover = 0.0f;
        if (input.hover && i < static_cast<int>(input.hover->amounts.size())) {
            hover = input.hover->amounts[static_cast<std::size_t>(i)];
        }

        const render::RectF cell = LaunchpadCellRect(input.grid, i);

        // Selection plate behind the icon, drawn before it so the artwork stays on
        // top. Sized to the cell so a hovered neighbour does not overlap it.
        if (i == input.selected || hover > 0.01f) {
            const float strength = std::max(hover, i == input.selected ? 1.0f : 0.0f);
            render::ColorF selection = kSelectionColor;
            selection.a *= strength * fade;
            const render::RectF plate{cell.left + kSelectionInsetDip, cell.top,
                                      cell.right - kSelectionInsetDip,
                                      cell.bottom - kSelectionInsetDip};
            backend.FillRoundedRect(plate, kSelectionRadiusDip, selection);
        }

        // Icon, scaled about the cell's horizontal centre and the icon rect's centre
        // so a magnified icon grows in place rather than drifting toward a corner.
        const float edge = LaunchpadIconEdgeFor(input.grid, hover);
        const float center_x = cell.CenterX();
        const float center_y = cell.top + input.grid.icon_size_dip * 0.5f;
        const render::RectF icon_rect{center_x - edge * 0.5f, center_y - edge * 0.5f,
                                      center_x + edge * 0.5f, center_y + edge * 0.5f};

        if (tile.icon.valid()) {
            const render::BitmapView view{tile.icon.width, tile.icon.height,
                                          tile.icon.pixels.data()};
            backend.DrawBitmap(tile.icon_key, view, icon_rect, fade);
        } else {
            backend.StrokeRoundedRect(icon_rect, edge * 0.5f, kPlaceholderRingColor, 1.5f);
        }

        // Label. Always drawn, even while the icon is missing: the name is how the
        // user finds an app whose icon the shell would not hand over.
        if (!tile.app.display_name.empty()) {
            render::ColorF label = kLabelColor;
            label.a *= fade;
            backend.DrawLabel(tile.app.display_name.c_str(), LaunchpadLabelRect(input.grid, i),
                              kLabelSizeDip, label, render::TextAlign::Center);
        }
    }
}

void LaunchpadPainter::PaintDots(render::IRenderBackend& backend,
                                 const LaunchpadPaintInput& input) {
    if (!LaunchpadShowsPageDots(input.page_count)) return;

    const float fade = LaunchpadAnimationFade(input.progress);
    for (int page = 0; page < input.page_count; ++page) {
        const render::RectF dot = LaunchpadDotRect(input.grid, input.height_dip, page,
                                                   input.page_count);
        if (dot.Width() <= 0.0f) continue;
        render::ColorF color = page == input.page ? kDotColorActive : kDotColorIdle;
        color.a *= fade;
        backend.FillRoundedRect(dot, kLaunchpadDotDip * 0.5f, color);
    }
}

int LaunchpadPainter::HitTest(const LaunchpadGrid& grid, int items_on_page, float x_dip,
                              float y_dip) const {
    return LaunchpadHitTest(grid, items_on_page, x_dip, y_dip);
}

}  // namespace dockforge::dock
