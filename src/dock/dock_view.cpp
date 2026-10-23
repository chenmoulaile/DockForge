// DockForge - Dock tile model, geometry and painter (T-009).
#include "dock/dock_view.h"

#include "core/log.h"
#include "core/paths.h"

#include <algorithm>
#include <cmath>

namespace dockforge::dock {
namespace {

// Same construction the shell cache uses for its disk keys, so a tile's GPU
// identity and its cache identity agree without a second hashing scheme.
std::uint64_t Fnv1a16(const std::wstring& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t ch : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<std::uint16_t>(ch));
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

std::uint64_t IconKeyFor(const std::wstring& source) { return Fnv1a16(source); }

std::vector<DockTile> BuildTiles(const std::vector<shell::AppEntry>& apps,
                                 shell::IconCache& icons, int max_tiles, int icon_px) {
    std::vector<DockTile> tiles;
    if (max_tiles <= 0) return tiles;
    tiles.reserve(static_cast<std::size_t>(std::min(max_tiles, 64)));

    for (const auto& app : apps) {
        if (static_cast<int>(tiles.size()) >= max_tiles) break;

        DockTile tile;
        tile.app = app;
        const std::wstring source = shell::IconSourceFor(app);
        // A tile with no icon is still shown: a missing picture is a cosmetic
        // problem, a missing launcher is a functional one. The plate draws a
        // placeholder ring so the slot is visibly occupied rather than blank.
        icons.Get(source, icon_px, tile.icon);
        tile.icon_key = IconKeyFor(source);
        tiles.push_back(std::move(tile));
    }
    return tiles;
}

DockGeometry ComputeGeometry(const core::Config& config, int tile_count, float scale) {
    DockGeometry geometry;
    if (!(scale > 0.0f)) scale = 1.0f;
    geometry.scale = scale;
    geometry.tile_count = std::max(tile_count, 0);
    geometry.params = FromConfig(config.dock, scale);

    // The layout works in device pixels (C-05), the window is sized in DIPs. Going
    // through the same LayoutParams for both keeps the two from drifting: a dock
    // sized from `icon_size * scale` but laid out from `icon_size` would clip its
    // last icon at every non-100% DPI.
    const float content_px =
        BaseContentWidth(geometry.tile_count, geometry.params);
    geometry.content_width_dip = content_px / scale;
    geometry.plate_width_dip = geometry.content_width_dip + 2.0f * kPlatePaddingDip;
    geometry.plate_height_dip =
        static_cast<float>(std::max(config.dock.icon_size, 1)) + 2.0f * kPlatePaddingDip;
    geometry.window_width_dip = geometry.plate_width_dip;
    geometry.window_height_dip = geometry.plate_height_dip;
    return geometry;
}

bool DockPainter::Paint(ID2D1DeviceContext& dc, render::IRenderBackend& backend,
                        render::PlateRenderer& plate, const DockPaintInput& input) {
    if (!input.config || !input.style || !input.tiles) return false;
    const float scale = input.dpi > 0.0f ? input.dpi / 96.0f : 1.0f;
    const auto& config = *input.config;

    const int count = static_cast<int>(input.tiles->size());
    const auto geometry = ComputeGeometry(config, count, scale);
    const float width_dip = geometry.window_width_dip;
    const float height_dip = geometry.window_height_dip;

    // Plate first: the frost must not paint over the icons.
    const float radius = std::min(static_cast<float>(config.appearance.corner_radius),
                                  std::min(width_dip, height_dip) * 0.5f);
    const render::RectF panel{0.5f, 0.5f, width_dip - 0.5f, height_dip - 0.5f};
    if (!plate.Draw(dc, backend, panel, radius, *input.style, input.backdrop, input.backdrop_w,
                    input.backdrop_h, input.dpi)) {
        return false;
    }

    if (count == 0) {
        // An empty dock still shows its plate, otherwise the window looks like a
        // rendering failure instead of an empty dock.
        last_ = LayoutSummary{};
        return true;
    }

    // Layout in device pixels, then convert once. `mouse_x_dip` is DIP because the
    // window receives DIP-scaled coordinates from Windows; C-05 wants pixels.
    scratch_.resize(static_cast<std::size_t>(count));
    const float mouse_px = input.hover.mouse_x_dip * scale;
    LayoutDock(geometry.params, count, mouse_px, input.hover.active, scratch_.data(),
               scratch_.size(), &last_);

    const float icon_center_y = height_dip * 0.5f;
    for (int i = 0; i < count; ++i) {
        const IconLayout& icon = scratch_[static_cast<std::size_t>(i)];
        const DockTile& tile = (*input.tiles)[static_cast<std::size_t>(i)];

        // Layout centres are measured from the plate's left edge, so the padding
        // shifts the whole row inward.
        const float center_x_dip = kPlatePaddingDip + icon.center / scale;
        const float half_dip = icon.width / scale * 0.5f;
        const render::RectF slot{center_x_dip - half_dip, icon_center_y - half_dip,
                                 center_x_dip + half_dip, icon_center_y + half_dip};

        if (tile.icon.valid()) {
            const render::BitmapView view{tile.icon.width, tile.icon.height,
                                          tile.icon.pixels.data()};
            backend.DrawBitmap(tile.icon_key, view, slot, 1.0f);
        } else {
            // Placeholder: a faint ring where the icon would be. Silence here would
            // read as a broken dock rather than a missing shell icon.
            backend.StrokeRoundedRect(slot, half_dip, render::ColorF{0.62f, 0.66f, 0.74f, 0.35f},
                                      1.5f);
        }
    }
    return true;
}

}  // namespace dockforge::dock
