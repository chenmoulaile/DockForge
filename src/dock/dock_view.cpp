// DockForge - Dock tile model, geometry and painter (T-009, hover T-010).
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

// Widest the icon row can get for any pointer position, in DIP.
//
// The window is sized once from this so magnification never resizes it. Resizing
// on hover would move the window under the cursor (the row is centred), jittering
// the hit test and re-arming the backdrop capture region on every enter/leave.
//
// Swept rather than solved in closed form: the shift term sums over every icon, so
// the maximum is not simply base_width plus one icon's growth. A sweep is exact for
// the config in hand; it costs a few hundred layout evaluations and runs only when
// the config, tile count or DPI changes, never per frame.
float MaxRowWidthDip(int tile_count, const LayoutParams& params, float scale,
                     float base_width_px) {
    if (tile_count <= 0 || !(scale > 0.0f)) return base_width_px;
    const int steps = std::max(32, std::min(256, static_cast<int>(base_width_px)));
    float widest_px = base_width_px;

    std::vector<IconLayout> scratch(static_cast<std::size_t>(tile_count));
    for (int i = 0; i <= steps; ++i) {
        const float mouse = base_width_px * static_cast<float>(i) / static_cast<float>(steps);
        LayoutSummary summary{};
        LayoutDock(params, tile_count, mouse, true, scratch.data(), scratch.size(), &summary);
        widest_px = std::max(widest_px, summary.max_extent - summary.min_extent);
    }
    return widest_px / scale;
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
    const float content_px = BaseContentWidth(geometry.tile_count, geometry.params);
    geometry.content_width_dip = content_px / scale;

    const float icon_dip = static_cast<float>(std::max(config.dock.icon_size, 1));
    // The plate keeps its idle height while icons magnify, the way macOS does: the
    // icons lift out of the dock and overhang the background, which reads as growth
    // rather than the dock inflating. The plate height therefore does not depend on
    // hover at all.
    geometry.plate_height_dip = icon_dip + 2.0f * kPlatePaddingDip;

    geometry.plate_width_dip = geometry.content_width_dip + 2.0f * kPlatePaddingDip;
    // Width has to hold the widest row the pointer can produce, not the idle row: a
    // 1.8x icon with its neighbours pushed aside needs more room than the idle slots.
    const float widest_dip = MaxRowWidthDip(geometry.tile_count, geometry.params, scale,
                                            content_px);
    geometry.window_width_dip =
        std::max(geometry.plate_width_dip, widest_dip + 2.0f * kPlatePaddingDip);

    // The window reserves room for the tallest icon, which overhangs above the
    // plate. The band between the window top and the plate is transparent dead
    // space, excluded from the window region so clicks pass to the app underneath
    // (spike conclusion 3: SetWindowRgn clips DComp content and moves hit-testing).
    geometry.window_height_dip =
        icon_dip * std::max(geometry.params.magnification, 1.0f) + 2.0f * kPlatePaddingDip;

    // Icons bottom-align here and grow upward, so the row never dips below the dock.
    // Computed once instead of in both Paint and the indicator pass: the two must
    // agree to the pixel or the dots drift off the icons they belong to.
    geometry.baseline_dip = geometry.window_height_dip - kPlatePaddingDip;
    return geometry;
}

std::vector<float> IdleCentersDip(const DockGeometry& geometry, float scale) {
    std::vector<float> centers;
    if (!(scale > 0.0f)) return centers;
    const int count = std::max(geometry.tile_count, 0);
    centers.reserve(static_cast<std::size_t>(count));
    const float origin_dip = (geometry.window_width_dip - geometry.content_width_dip) * 0.5f;
    const float pitch_px =
        static_cast<float>(geometry.params.icon_size + geometry.params.gap);
    const float half_px = static_cast<float>(geometry.params.icon_size) * 0.5f;
    for (int i = 0; i < count; ++i) {
        centers.push_back(origin_dip + (i * pitch_px + half_px) / scale);
    }
    return centers;
}

bool AdvanceHover(DockHover& hover, double delta_ms) {
    if (!(delta_ms > 0.0)) return false;
    const float target = hover.active ? 1.0f : 0.0f;
    const float step = static_cast<float>(delta_ms) / 1000.0f * kHoverRampPerSecond;
    float next = hover.intensity;
    if (next < target) {
        next = std::min(target, next + step);
    } else if (next > target) {
        next = std::max(target, next - step);
    }
    if (next == hover.intensity) return false;
    hover.intensity = next;
    return true;
}

const LayoutSummary& DockPainter::UpdateLayout(const DockGeometry& geometry,
                                              const DockHover& hover, int tile_count) {
    const int count = std::max(tile_count, 0);
    baseline_dip_ = geometry.baseline_dip;
    geometry_ = geometry;
    if (count == 0) {
        last_ = LayoutSummary{};
        // Cleared so HitTest cannot report a slot that Paint would not draw after the
        // dock loses its last tile.
        scratch_.clear();
        row_origin_dip_ = 0.0f;
        return last_;
    }

    const float intensity = std::clamp(hover.intensity, 0.0f, 1.0f);
    LayoutParams params = geometry.params;
    params.magnification = 1.0f + (params.magnification - 1.0f) * intensity;

    const float row_origin_dip =
        (geometry.window_width_dip - geometry.content_width_dip) * 0.5f;
    row_origin_dip_ = row_origin_dip;
    scratch_.resize(static_cast<std::size_t>(count));
    const float mouse_px = (hover.mouse_x_dip - row_origin_dip) * geometry.scale;
    LayoutDock(params, count, mouse_px, hover.active && intensity > 0.0f, scratch_.data(),
               scratch_.size(), &last_);
    return last_;
}

int DockPainter::HitTest(float x_dip, float scale) const {
    if (scratch_.empty() || !(scale > 0.0f)) return -1;
    for (std::size_t i = 0; i < scratch_.size(); ++i) {
        const IconLayout& icon = scratch_[i];
        const float half_dip = icon.width / scale * 0.5f;
        // Same conversion Paint uses: the layout is dock-local device pixels, the
        // click is dock-local DIPs. Adding the row origin here rather than in the
        // caller keeps the two from disagreeing about where the row starts.
        const float center_dip = row_origin_dip_ + icon.center / scale;
        if (x_dip >= center_dip - half_dip && x_dip <= center_dip + half_dip) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<float> DockPainter::IconCentersDip(float scale) const {
    std::vector<float> centers;
    if (!(scale > 0.0f)) return centers;
    centers.reserve(scratch_.size());
    for (const IconLayout& icon : scratch_) {
        centers.push_back(row_origin_dip_ + icon.center / scale);
    }
    return centers;
}

std::vector<float> DockPainter::IdleCentersDip(float scale) const {
    // Analytic, not a layout run: at rest every icon has scale 1 and zero shift, so the
    // centres are just a fixed pitch. Computing them from geometry_ rather than from
    // scratch_ is the whole point - scratch_ holds the magnified row.
    return dockforge::dock::IdleCentersDip(geometry_, scale);
}

bool DockPainter::Paint(ID2D1DeviceContext& dc, render::IRenderBackend& backend,
                        render::PlateRenderer& plate, const DockPaintInput& input) {
    if (!input.config || !input.style || !input.tiles) return false;
    const float scale = input.dpi > 0.0f ? input.dpi / 96.0f : 1.0f;
    const auto& config = *input.config;
    // Geometry is computed by the caller and passed in. It must NOT be recomputed
    // here: ComputeGeometry sweeps up to 256 layouts to find the widest row, and
    // this function runs on every hover frame (C-05a rule 3).
    const DockGeometry& geometry = input.geometry;

    const int count = static_cast<int>(input.tiles->size());
    const float width_dip = geometry.window_width_dip;
    const float height_dip = geometry.window_height_dip;
    const float plate_height_dip = geometry.plate_height_dip;

    // The plate sits on the window's bottom edge; the reserved band above it is
    // where magnified icons overhang into.
    const float plate_top_dip = height_dip - plate_height_dip;
    const float radius = std::min(static_cast<float>(config.appearance.corner_radius),
                                  std::min(width_dip, plate_height_dip) * 0.5f);
    const render::RectF panel{0.5f, plate_top_dip + 0.5f, width_dip - 0.5f,
                              height_dip - 0.5f};
    if (!plate.Draw(dc, backend, panel, radius, *input.style, input.backdrop,
                    input.backdrop_w, input.backdrop_h, input.dpi)) {
        return false;
    }

    if (count == 0) {
        // An empty dock still shows its plate, otherwise the window looks like a
        // rendering failure instead of an empty dock.
        UpdateLayout(geometry, input.hover, 0);
        return true;
    }

    // Deterministic and idempotent: same geometry + same hover produces the same
    // layout, so the region applied just before this call describes exactly the row
    // painted here. O(n) with no allocation after the first resize.
    UpdateLayout(geometry, input.hover, count);

    // Layout in device pixels, then convert once. `mouse_x_dip` is DIP because the
    // window receives DIP-scaled coordinates from Windows; C-05 wants pixels.
    const float row_origin_dip = (width_dip - geometry.content_width_dip) * 0.5f;

    // Icons are bottom-aligned to the plate's inner edge and grow upward, so the
    // row never dips below the dock and the tallest icon defines the top. The
    // baseline comes from the geometry so the indicator pass below lands on the same
    // line without recomputing it.
    const float baseline_dip = geometry.baseline_dip;

    // A tile being dragged is drawn as a ghost at the pointer instead of in its slot,
    // and its slot is left empty so the row reads as "picked up". Drawing it twice
    // (in the slot and at the pointer) would look like a duplicate, and drawing it in
    // the slot only would make the drag invisible until release.
    const DragState* drag = input.drag;
    const bool dragging = drag != nullptr && drag->source_index >= 0 &&
                          drag->source_index < count &&
                          (drag->phase == DragPhase::Dragging ||
                           drag->phase == DragPhase::Ejecting);
    const int drag_source = dragging ? drag->source_index : -1;
    const int drag_insert = dragging ? drag->insert_index : -1;

    // Edges of the slot the insertion line attaches to, filled in during the loop.
    float insert_left_dip = 0.0f;
    float insert_right_dip = 0.0f;

    for (int i = 0; i < count; ++i) {
        const IconLayout& icon = scratch_[static_cast<std::size_t>(i)];
        const DockTile& tile = (*input.tiles)[static_cast<std::size_t>(i)];

        const float center_x_dip = row_origin_dip + icon.center / scale;
        const float half_dip = icon.width / scale * 0.5f;
        const render::RectF slot{center_x_dip - half_dip, baseline_dip - icon.width / scale,
                                 center_x_dip + half_dip, baseline_dip};
        if (i == drag_insert) {
            insert_left_dip = slot.left;
            insert_right_dip = slot.right;
        }

        if (i != drag_source) {
            if (tile.icon.valid()) {
                const render::BitmapView view{tile.icon.width, tile.icon.height,
                                              tile.icon.pixels.data()};
                backend.DrawBitmap(tile.icon_key, view, slot, 1.0f);
            } else {
                // Placeholder: a faint ring where the icon would be. Silence here
                // would read as a broken dock rather than a missing shell icon.
                backend.StrokeRoundedRect(
                    slot, half_dip, render::ColorF{0.62f, 0.66f, 0.74f, 0.35f}, 1.5f);
            }
        }

        // Running indicator, centred in the plate's bottom padding.
        //
        // Drawn inside the padding rather than below the plate because the plate's
        // bottom edge is the window's bottom edge: anything lower would fall outside
        // both the plate and the window region, and SetWindowRgn would clip it.
        // Suppressed for the dragged tile: a dot hanging under an empty slot reads as
        // a running app with no icon.
        if (tile.run_state != TileRunState::Idle && i != drag_source) {
            const bool front = tile.run_state == TileRunState::Foreground;
            const float dot = kIndicatorDotDip;
            // Vertically centred in the 10 DIP padding that sits under the icons.
            const float dot_y = baseline_dip + kPlatePaddingDip * 0.5f;
            const float alpha = front ? kIndicatorOpacityForeground : kIndicatorOpacityRunning;
            // The foreground dot is drawn from the icon's centre, the running dot
            // slightly wider. Same geometry otherwise: two different sizes would make
            // the row read as two rows.
            const float dot_w = front ? dot : dot * 0.85f;
            const render::RectF mark{center_x_dip - dot_w * 0.5f, dot_y - dot * 0.5f,
                                     center_x_dip + dot_w * 0.5f, dot_y + dot * 0.5f};
            backend.FillRoundedRect(mark, dot_w * 0.5f,
                                    render::ColorF{0.92f, 0.94f, 0.98f, alpha});
        }
    }

    // External drag (T-014): a caret at the slot the payload would land in.
    //
    // Drawn for both the accepting and the rejecting case, in different colours. The
    // cursor already carries the source application's "no drop" badge, but that badge
    // is drawn by a process the dock does not control and disappears the moment the
    // pointer is over a window that reports DROPEFFECT_NONE - which reads as "the dock
    // is fine with this" right up until the release does nothing.
    if (input.drop != nullptr && input.drop->active) {
        const float icon_dip = static_cast<float>(std::max(config.dock.icon_size, 1));
        // The caret sits on the REST grid, not the magnified one, because that is the
        // grid the drop target resolves the slot against (C-16d: the row settles while
        // an external drag is over the dock). Deriving it analytically also keeps this
        // off the allocation path: a DragOver fires dozens of times per second and
        // building a centre vector here would allocate on every one.
        const int slot = std::clamp(input.drop->insert_index, 0, count);
        const float pitch_dip =
            static_cast<float>(geometry.params.icon_size + geometry.params.gap) / scale;
        const float caret_x_dip = row_origin_dip + static_cast<float>(slot) * pitch_dip;
        const float half = icon_dip * kDropLineHeightFactor * 0.5f;
        const float mid = baseline_dip - icon_dip * 0.5f;
        const render::RectF caret{caret_x_dip - kDropCaretWidthDip * 0.5f, mid - half,
                                  caret_x_dip + kDropCaretWidthDip * 0.5f, mid + half};
        // Where the caret actually lands, in DIP and in device pixels. The image
        // measurement and this line have to agree; when they did not, the caret was
        // being read off the wrong column and the slot was blamed instead.
        DF_TRACE("dock", "drop caret slot=%d origin=%.2fdip pitch=%.2fdip x=%.2fdip px=%.1f "
                         "band=%.2f..%.2f dip",
                 slot, static_cast<double>(row_origin_dip), static_cast<double>(pitch_dip),
                 static_cast<double>(caret_x_dip), static_cast<double>(caret_x_dip * scale),
                 static_cast<double>(mid - half), static_cast<double>(mid + half));
        backend.FillRoundedRect(caret, kDropCaretWidthDip * 0.5f,
                                input.drop->acceptable ? kDropLineAcceptColor
                                                       : kDropLineRejectColor);
    }

    if (!dragging) return true;

    // Insertion line: a short vertical caret at the edge of the destination slot.
    //
    // Placed on the side the tile is arriving from (left edge when moving left, right
    // edge when moving right) so the line sits where the tile's leading edge will
    // land. Drawing it on a fixed side would put it one slot away from the icon that
    // is about to move.
    if (drag_insert != drag_source && drag_insert >= 0 && drag_insert < count) {
        const float x = drag_insert < drag_source ? insert_left_dip : insert_right_dip;
        const float icon_dip = static_cast<float>(std::max(config.dock.icon_size, 1));
        const float half = icon_dip * kDropLineHeightFactor * 0.5f;
        const float mid = baseline_dip - icon_dip * 0.5f;
        const render::RectF line{x - kDropLineWidthDip * 0.5f, mid - half,
                                 x + kDropLineWidthDip * 0.5f, mid + half};
        // Only drawn while the tile would actually move. A line at the source slot
        // would claim a reorder that release will not perform (End() reports Click
        // when insert == source).
        backend.FillRoundedRect(line, kDropLineWidthDip * 0.5f,
                                render::ColorF{0.92f, 0.94f, 0.98f, 0.85f});
    }

    // Ghost: the lifted tile, centred on the pointer.
    //
    // Uses the idle icon size rather than the magnified one. The magnified size
    // belongs to the slot the icon sits in, and the ghost is no longer in a slot;
    // keeping it at 1x also makes it visibly smaller than the row it came from, which
    // is what tells the user it has been detached.
    const DockTile& source = (*input.tiles)[static_cast<std::size_t>(drag_source)];
    if (source.icon.valid()) {
        const float icon_dip = static_cast<float>(std::max(config.dock.icon_size, 1));
        const float half = icon_dip * 0.5f;
        const render::RectF ghost{drag->pointer_x_dip - half, drag->pointer_y_dip - half,
                                  drag->pointer_x_dip + half, drag->pointer_y_dip + half};
        const render::BitmapView view{source.icon.width, source.icon.height,
                                      source.icon.pixels.data()};
        // Fading with the eject hold is the only feedback the confirmation has. The
        // dock never takes focus, so there is no dialog and no title-bar cue: if the
        // ghost did not fade, "hold to remove" would be indistinguishable from "the
        // dock has stopped responding". Fading to 35% leaves the artwork readable
        // right up to the moment it disappears, so the user can still see which tile
        // they are about to lose.
        const float opacity =
            kDragGhostOpacity * (1.0f - 0.65f * std::clamp(drag->eject_progress, 0.0f, 1.0f));
        backend.DrawBitmap(source.icon_key, view, ghost, opacity);
    }

    return true;
}

}  // namespace dockforge::dock
