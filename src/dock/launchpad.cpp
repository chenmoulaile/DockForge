#include "dock/launchpad.h"

#include "core/log.h"
#include "core/paths.h"
#include "platform/display.h"
#include "platform/window.h"
#include "shell/app_launcher.h"
#include "shell/app_resolver.h"

#include <algorithm>
#include <cmath>

namespace dockforge::dock {
namespace {

constexpr char kLogModule[] = "launchpad";

// Cap on the icon extraction size. 256px is the largest the Dock extracts and the
// largest a shell icon source reliably produces; asking for more costs time and yields
// an upscale.
constexpr int kMaxIconPx = 256;

// Material for the overlay plate.
//
// Solid is deliberate, not a degradation: the Launchpad is a full-screen surface, so
// capturing a 4K desktop for the plate costs a flat ~78ms per frame (evidence/T-005),
// which is 4.7 frames of the 16.7ms budget. The scrim alpha in the paint path supplies
// the visual separation instead.
core::MaterialLevel LaunchpadMaterial(const core::Config& config) {
    (void)config;
    return core::MaterialLevel::Solid;
}

}  // namespace

const char* ToString(LaunchpadPhase phase) {
    switch (phase) {
        case LaunchpadPhase::Closed:
            return "closed";
        case LaunchpadPhase::Opening:
            return "opening";
        case LaunchpadPhase::Open:
            return "open";
        case LaunchpadPhase::Closing:
            return "closing";
    }
    return "unknown";
}

Launchpad::Launchpad() = default;

Launchpad::~Launchpad() { DestroyOverlays(); }

bool Launchpad::Initialize(HINSTANCE instance, const core::Config& config,
                           shell::IconCache& icons, const LaunchpadOptions& options) {
    instance_ = instance;
    config_ = config;
    icons_ = &icons;
    options_ = options;

    if (!platform::OverlayWindow::RegisterWindowClass(instance)) {
        DF_ERROR(kLogModule, "overlay window class registration failed");
        return false;
    }

    std::vector<platform::DisplayInfo> displays = platform::EnumerateDisplays();
    if (displays.empty()) {
        DF_WARN(kLogModule, "no displays enumerated; Launchpad disabled");
        return false;
    }
    // The Dock applies DOCKFORGE_DPI_OVERRIDE to its own target display in main.cpp, but
    // the Launchpad enumerates displays itself and would otherwise ignore the override -
    // the same value would then mean two different things depending on which surface you
    // looked at, and the 150% DoD could not be exercised on a 200% panel. Honour it here
    // through the same platform::Window accessor the Dock uses.
    if (const UINT forced = platform::Window::DpiOverride(); forced != 0) {
        for (platform::DisplayInfo& display : displays) display.effective_dpi = forced;
        DF_INFO(kLogModule, "dpi override %u applied to %zu display(s)", forced,
                displays.size());
    }

    const core::MaterialLevel tier = LaunchpadMaterial(config);
    overlays_.clear();
    overlays_.reserve(displays.size());

    for (const platform::DisplayInfo& display : displays) {
        auto overlay = std::make_unique<LaunchpadOverlay>();
        overlay->display = display;

        // Full monitor rect, not the work area: a Launchpad covers the taskbar too, the
        // same way Mission Control does. Using `work` would leave a strip of desktop
        // visible along the bottom and make the scrim look mis-sized.
        const RECT& bounds = display.monitor;
        if (!overlay->window.Create(instance, bounds, display.effective_dpi,
                                    L"DockForge Launchpad")) {
            DF_WARN(kLogModule, "overlay create failed for %s",
                    core::WideToUtf8(display.device).c_str());
            continue;
        }
        if (!overlay->surface.Initialize(overlay->window.Handle())) {
            DF_WARN(kLogModule, "composition surface failed for %s",
                    core::WideToUtf8(display.device).c_str());
            overlay->window.Destroy();
            continue;
        }
        const int width_px = overlay->window.WidthPx();
        const int height_px = overlay->window.HeightPx();
        if (!overlay->surface.Resize(width_px, height_px)) {
            DF_WARN(kLogModule, "surface resize failed for %s (%dx%d)",
                    core::WideToUtf8(display.device).c_str(), width_px, height_px);
            overlay->surface.Shutdown();
            overlay->window.Destroy();
            continue;
        }

        overlay->backend = render::CreateD2DBackend();
        if (!overlay->backend || !overlay->backend->Initialize()) {
            DF_WARN(kLogModule, "render backend failed for %s",
                    core::WideToUtf8(display.device).c_str());
            overlay->surface.Shutdown();
            overlay->window.Destroy();
            continue;
        }

        overlay->ready = true;
        overlays_.push_back(std::move(overlay));
    }

    if (overlays_.empty()) {
        DF_ERROR(kLogModule, "no overlay could be created; Launchpad disabled");
        return false;
    }

    hovers_.resize(overlays_.size());
    const std::size_t count = overlays_.size();
    for (std::size_t i = 0; i < count; ++i) {
        platform::OverlayCallbacks callbacks;
        const std::size_t index = i;

        // Every callback captures the display index, never a pointer into overlays_:
        // the vector can still grow, and a captured address would dangle.
        callbacks.pointer_moved = [this, index](float x, float y) {
            PointerMoved(index, x, y);
        };
        callbacks.pointer_left = [this, index] { PointerLeft(index); };
        callbacks.clicked = [this, index](float x, float y, bool) {
            ClickAt(index, x, y);
        };
        callbacks.key_pressed = [this](unsigned int vk) { KeyPressed(vk); };
        callbacks.dismissed = [this] { Close(); };
        callbacks.destroyed = [this, index] {
            if (index < overlays_.size()) overlays_[index]->ready = false;
        };
        overlays_[i]->window.SetCallbacks(std::move(callbacks));
    }

    DF_INFO(kLogModule, "initialized %zu overlay(s), tier=%s", overlays_.size(),
            core::ToString(tier));
    return true;
}

void Launchpad::DestroyOverlays() {
    for (auto& overlay : overlays_) {
        // Order matters: the backend holds device-dependent resources created against
        // the surface's D2D device, so it goes first; the surface must go before the
        // window, because a DComp target bound to a dead HWND is a shutdown hang.
        if (overlay->backend) overlay->backend->Shutdown();
        overlay->backend.reset();
        overlay->plate.Reset();
        overlay->surface.Shutdown();
        overlay->window.Destroy();
        overlay->ready = false;
    }
    overlays_.clear();
    hovers_.clear();
    painted_page_ = -1;
}

void Launchpad::Shutdown() {
    watcher_.Stop();
    DestroyOverlays();
    tiles_.clear();
    tiles_stale_ = true;
    phase_ = LaunchpadPhase::Closed;
    progress_ = 0.0f;
}

void Launchpad::RebuildIndex(const core::Config& config) {
    const std::uint64_t start_ms = GetTickCount64();
    index_.Rebuild(config.launchpad);
    const shell::LaunchpadIndexStats& stats = index_.Stats();
    DF_INFO(kLogModule,
            "index rebuilt: items=%zu apps=%d accepted=%d extra=%d dup=%d skipped=%d "
            "pathfail=%d scan=%.1fms total=%.1fms",
            index_.Items().size(), stats.apps_folder_count, stats.apps_folder_accepted,
            stats.extra_path_count, stats.duplicate_count, stats.skipped, stats.path_failures,
            stats.scan_ms, static_cast<double>(GetTickCount64() - start_ms));

    // The watcher is restarted with the current roots so a change to `extra_paths`
    // between two rebuilds is picked up on the next settle rather than never.
    watcher_.Stop();
    const std::vector<std::wstring> roots = shell::DefaultWatchRoots(config.launchpad);
    if (!watcher_.Start(roots)) {
        // A Launchpad that refused to work because one directory vanished would be worse
        // than one that stops noticing changes: log and carry on (C-18).
        DF_WARN(kLogModule, "watcher could not start; falling back to rebuild on open");
    }
    tiles_stale_ = true;
}

int Launchpad::ItemCount() const { return static_cast<int>(index_.Items().size()); }

const LaunchpadOverlay* Launchpad::OverlayAt(std::size_t display_index) const {
    if (display_index >= overlays_.size()) return nullptr;
    return overlays_[display_index].get();
}

LaunchpadGrid Launchpad::GridForOverlay(const LaunchpadOverlay& overlay,
                                        const core::Config& config) const {
    LaunchpadGridInput input;
    input.width_dip = overlay.window.WidthDip();
    input.height_dip = overlay.window.HeightDip();
    input.icon_size = config.launchpad.icon_size;
    input.columns = config.launchpad.columns;
    input.page_size = config.launchpad.page_size;
    return ComputeLaunchpadGrid(input);
}

LaunchpadGrid Launchpad::GridFor(std::size_t display_index) const {
    if (display_index >= overlays_.size()) return LaunchpadGrid{};
    return GridForOverlay(*overlays_[display_index], config_);
}

int Launchpad::PageCount() const {
    if (overlays_.empty()) return 1;
    // The page count belongs to the grid, and each display can produce a different one.
    // Report the smallest so a page never exists on one screen and not another - the
    // page index is shared across displays because the Launchpad opens as one gesture.
    int pages = -1;
    for (const auto& overlay : overlays_) {
        const LaunchpadGrid grid = GridForOverlay(*overlay, config_);
        const int count = LaunchpadPageCount(ItemCount(), grid);
        pages = (pages < 0) ? count : std::min(pages, count);
    }
    return std::max(1, pages < 0 ? 1 : pages);
}

void Launchpad::EnsureTiles(const core::Config& config) {
    if (!tiles_stale_ && !tiles_.empty()) return;

    // Extraction size follows the largest display scale so a 200% monitor does not get
    // an upscaled icon. `icon_size` is DIP; the shell source wants pixels.
    float max_scale = 1.0f;
    for (const auto& overlay : overlays_) {
        max_scale = std::max(max_scale, overlay->window.Scale());
    }
    int icon_px = static_cast<int>(std::lround(static_cast<double>(config.launchpad.icon_size) *
                                               static_cast<double>(max_scale)));
    icon_px = std::clamp(icon_px, 32, kMaxIconPx);

    const std::uint64_t start_ms = GetTickCount64();
    tiles_ = BuildLaunchpadTiles(index_.Items(), *icons_, options_.max_icon_tiles, icon_px);
    DF_INFO(kLogModule, "tiles built: %zu of %zu items at %dpx in %.1fms", tiles_.size(),
            index_.Items().size(), icon_px,
            static_cast<double>(GetTickCount64() - start_ms));
    tiles_stale_ = false;
}

void Launchpad::WarmTiles(const core::Config& config) {
    config_ = config;
    EnsureTiles(config);
}

void Launchpad::Open(const core::Config& config) {
    config_ = config;
    if (phase_ != LaunchpadPhase::Closed) return;
    if (overlays_.empty()) {
        DF_WARN(kLogModule, "open requested with no overlay");
        return;
    }

    if (index_.Empty() || index_stale_) {
        // The first open pays the scan; later opens pay it only when the watcher saw a
        // change while the Launchpad was closed. main.cpp also warms the index at
        // startup, so the very first open is normally free.
        RebuildIndex(config);
        index_stale_ = false;
    }
    EnsureTiles(config);

    page_ = ClampLaunchpadPage(page_, ItemCount(), GridFor(0));
    progress_ = 0.0f;
    phase_ = LaunchpadPhase::Opening;
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        overlays_[i]->window.Show();
        hovers_[i] = LaunchpadHover{};
    }
    hover_display_ = -1;
    // The grid is logged because it is the C-18 proof: columns came from the adaptive
    // formula (or the config override), rows from ceil(page_size / columns), and the
    // probe predicts cell centres from these numbers rather than re-deriving the layout.
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        const LaunchpadGrid grid = GridFor(i);
        DF_INFO(kLogModule,
                "grid[%zu] columns=%d rows=%d per_page=%d cell=%.1fx%.1f origin=%.1f,%.1f "
                "icon=%.1f viewport=%.0fx%.0f",
                i, grid.columns, grid.rows, grid.per_page, grid.cell_width_dip,
                grid.cell_height_dip, grid.origin_x_dip, grid.origin_y_dip,
                grid.icon_size_dip, overlays_[i]->window.WidthDip(),
                overlays_[i]->window.HeightDip());
    }
    DF_INFO(kLogModule, "open items=%d pages=%d page=%d", ItemCount(), PageCount(), page_);
    Repaint();
}

void Launchpad::Close() {
    if (phase_ == LaunchpadPhase::Closed || phase_ == LaunchpadPhase::Closing) return;
    phase_ = LaunchpadPhase::Closing;
    DF_DEBUG(kLogModule, "closing");
}

bool Launchpad::Tick(double delta_ms, const core::Config& config) {
    bool dirty = false;

    // The frame clock keeps running while the dock is idle, so the first tick after a
    // long quiet stretch carries the whole gap as its delta. Measured: the Launchpad was
    // opened 19s after startup, the first tick reported delta 19000ms, and the 180ms
    // open animation finished inside one frame - the grid appeared with no animation at
    // all. Clamping the step means a stale delta can cost at most a few frames of
    // animation rather than skipping it.
    constexpr double kMaxAnimationStepMs = 64.0;  // four 60Hz frames
    const double animation_delta = std::min(delta_ms, kMaxAnimationStepMs);

    // A settled filesystem change invalidates the index. The rebuild is deferred to the
    // next Open(): a scan is 360-1045ms (evidence/T-040), and blocking the dock's frame
    // loop for that long while the Launchpad is not even visible is a freeze the user
    // pays for and cannot see the cause of.
    const double now_ms = static_cast<double>(GetTickCount64());
    if (watcher_.ConsumeChanged(now_ms)) {
        if (phase_ == LaunchpadPhase::Closed) {
            index_stale_ = true;
            DF_INFO(kLogModule, "watched directories changed; rebuild deferred to next open");
        } else {
            index_stale_ = true;
            DF_DEBUG(kLogModule, "watched directories changed; rebuild deferred until close");
        }
    }

    if (phase_ == LaunchpadPhase::Opening) {
        progress_ = AdvanceLaunchpadProgress(progress_, static_cast<float>(animation_delta),
                                             kLaunchpadOpenMs);
        if (progress_ >= 1.0f) {
            progress_ = 1.0f;
            phase_ = LaunchpadPhase::Open;
            DF_INFO(kLogModule, "open complete");
        }
        dirty = true;
    } else if (phase_ == LaunchpadPhase::Closing) {
        progress_ = AdvanceLaunchpadProgress(progress_, -static_cast<float>(animation_delta),
                                             kLaunchpadCloseMs);
        if (progress_ <= 0.0f) {
            progress_ = 0.0f;
            phase_ = LaunchpadPhase::Closed;
            for (auto& overlay : overlays_) overlay->window.Hide();
            DF_INFO(kLogModule, "closed");
        }
        dirty = true;
    }

    if (phase_ != LaunchpadPhase::Closed) {
        const int cells = LaunchpadPageItemCount(ItemCount(), GridFor(0), page_);
        for (std::size_t i = 0; i < hovers_.size(); ++i) {
            // Only the display under the pointer can have an active cell; the rest decay.
            LaunchpadHover& hover = hovers_[i];
            if (static_cast<int>(i) != hover_display_) hover.active = false;
            if (AdvanceLaunchpadHover(hover, cells, animation_delta)) dirty = true;
        }
    }

    // The overlays are their own windows with their own composition surfaces, and their
    // WM_PAINT draws nothing by design (the pixels come from DComp). So a frame of the
    // Launchpad animation is only ever produced here: returning `dirty` to the caller
    // invalidates the *dock* window, which would animate nothing. Measured: with the
    // repaint left to the caller, the overlay showed exactly one frame, the one painted
    // with progress_ = 0, which is fully transparent - the grid never appeared at all
    // (A/B screenshot ratio 0.0044, indistinguishable from closed).
    if (dirty && phase_ != LaunchpadPhase::Closed) {
        Repaint();
    }

    return dirty;
}

void Launchpad::ApplyPageClamp() {
    if (overlays_.empty()) {
        page_ = 0;
        return;
    }
    const int clamped = ClampLaunchpadPage(page_, ItemCount(), GridFor(0));
    if (clamped != page_) {
        DF_DEBUG(kLogModule, "page clamped %d -> %d", page_, clamped);
        page_ = clamped;
    }
}

void Launchpad::PaintOverlay(std::size_t display_index) {
    LaunchpadOverlay& overlay = *overlays_[display_index];
    if (!overlay.ready) return;

    const LaunchpadGrid grid = GridForOverlay(overlay, config_);
    const int items_on_page = LaunchpadPageItemCount(ItemCount(), grid, page_);

    LaunchpadPaintInput input;
    input.tiles = &tiles_;
    input.grid = grid;
    input.page = page_;
    input.page_count = LaunchpadPageCount(ItemCount(), grid);
    input.items_on_page = items_on_page;
    input.width_dip = overlay.window.WidthDip();
    input.height_dip = overlay.window.HeightDip();
    input.dpi = static_cast<float>(overlay.window.Dpi());
    input.progress = progress_;
    input.hover = display_index < hovers_.size() ? &hovers_[display_index] : nullptr;
    input.selected = -1;

    const float dpi = static_cast<float>(overlay.window.Dpi());
    // The page actually handed to the painter, logged only when it changes. Measured: a
    // "page 1" command logged page -> 1 while the grid on screen stayed on page 0, and
    // nothing in the log said which page the frame was drawn with - this closes that gap.
    if (painted_page_ != input.page) {
        DF_DEBUG(kLogModule, "paint[%zu] page=%d of %d items_on_page=%d progress=%.3f",
                 display_index, input.page, input.page_count, input.items_on_page,
                 static_cast<double>(input.progress));
        painted_page_ = input.page;
    }
    const bool painted = overlay.surface.Paint(
        dpi, [&](ID2D1DeviceContext& dc,
                 const platform::CompositionSurface::PaintInfo&) {
            overlay.backend->BeginFrame(dc, dpi);
            painter_.Paint(dc, *overlay.backend, input);
            overlay.backend->EndFrame();
        });
    if (!painted) {
        DF_WARN(kLogModule, "surface paint failed");
        return;
    }
    overlay.surface.Commit();
}

void Launchpad::Repaint() {
    if (phase_ == LaunchpadPhase::Closed) return;
    for (std::size_t i = 0; i < overlays_.size(); ++i) PaintOverlay(i);
}

const shell::LaunchpadItem* Launchpad::ClickAt(std::size_t display_index, float x_dip,
                                               float y_dip) {
    if (display_index >= overlays_.size()) return nullptr;
    if (phase_ == LaunchpadPhase::Closed || phase_ == LaunchpadPhase::Closing) return nullptr;

    const LaunchpadGrid grid = GridFor(display_index);
    const int items_on_page = LaunchpadPageItemCount(ItemCount(), grid, page_);
    const int cell = painter_.HitTest(grid, items_on_page, x_dip, y_dip);
    if (cell < 0) {
        // A click on the scrim (outside every cell) dismisses, the way every full-screen
        // launcher behaves. A click on the grid plate between cells also lands here; the
        // grid is one surface, and treating a near-miss as "stay open" would feel stuck.
        Close();
        return nullptr;
    }

    const int item_index = page_ * grid.per_page + cell;
    if (item_index < 0 || item_index >= static_cast<int>(index_.Items().size())) return nullptr;
    const shell::LaunchpadItem& item = index_.Items()[static_cast<std::size_t>(item_index)];

    const shell::LaunchResult result = shell::Launch(item.app);
    DF_INFO(kLogModule, "launch %s -> %s route=%s pid=%u",
            core::WideToUtf8(item.app.display_name).c_str(), ToString(result.error),
            result.route, result.process_id);
    Close();
    return &item;
}

void Launchpad::PointerMoved(std::size_t display_index, float x_dip, float y_dip) {
    if (display_index >= overlays_.size() || phase_ == LaunchpadPhase::Closed) return;
    hover_display_ = static_cast<int>(display_index);

    const LaunchpadGrid grid = GridFor(display_index);
    const int items_on_page = LaunchpadPageItemCount(ItemCount(), grid, page_);
    const int cell = painter_.HitTest(grid, items_on_page, x_dip, y_dip);

    LaunchpadHover& hover = hovers_[display_index];
    hover.active = cell >= 0;
    hover.index_on_page = cell;
}

void Launchpad::PointerLeft(std::size_t display_index) {
    if (display_index >= hovers_.size()) return;
    hovers_[display_index].active = false;
    hovers_[display_index].index_on_page = -1;
    if (hover_display_ == static_cast<int>(display_index)) hover_display_ = -1;
}

void Launchpad::KeyPressed(unsigned int vk) {
    switch (vk) {
        case VK_LEFT:
        case VK_PRIOR:  // Page Up
            --page_;
            ApplyPageClamp();
            DF_DEBUG(kLogModule, "page -> %d", page_);
            Repaint();
            break;
        case VK_RIGHT:
        case VK_NEXT:  // Page Down
            ++page_;
            ApplyPageClamp();
            DF_DEBUG(kLogModule, "page -> %d", page_);
            Repaint();
            break;
        case VK_HOME:
            page_ = 0;
            Repaint();
            break;
        case VK_END:
            page_ = PageCount() - 1;
            ApplyPageClamp();
            Repaint();
            break;
        default:
            break;
    }
}

}  // namespace dockforge::dock
