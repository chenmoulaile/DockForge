#include "dock/launchpad.h"

#include "core/log.h"
#include "core/paths.h"
#include "dock/launchpad_folders.h"
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

// A press becomes a drag once the pointer moves this far from the press point. Small
// enough that a deliberate drag never reads as a click, large enough that a sloppy
// click does not reorder the grid.
constexpr float kDragThresholdDip = 14.0f;

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
        callbacks.pressed = [this, index](float x, float y) { PressAt(index, x, y); };
        callbacks.released = [this, index](float x, float y) { ReleaseAt(index, x, y); };
        callbacks.key_pressed = [this](unsigned int vk) { KeyPressed(vk); };
        callbacks.character = [this](wchar_t ch) { Character(ch); };
        callbacks.dismissed = [this] { Escape(); };
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

int Launchpad::VisibleCount() const {
    if (!search_query_.empty()) return static_cast<int>(visible_.size());
    if (!current_folder_id_.empty()) return static_cast<int>(visible_.size());
    return static_cast<int>(index_.Items().size());
}

bool Launchpad::HasFolders() const {
    return !config_.launchpad.folders.empty();
}

void Launchpad::SetSearchQuery(const std::wstring& text) {
    if (text == search_query_) return;
    search_query_ = text;
    // A query change invalidates the folder context: the user is looking for one app,
    // and a grid that silently kept the folder open would search only inside it.
    current_folder_id_.clear();
    caret_folder_id_.clear();
    RefreshVisible();
    page_ = 0;
    ApplyPageClamp();
    DF_DEBUG(kLogModule, "search '%s' -> %d item(s)", core::WideToUtf8(text).c_str(),
             VisibleCount());
    Repaint();
}

void Launchpad::RefreshVisible() {
    visible_.clear();
    if (!search_query_.empty()) {
        visible_ = index_.Filter(search_query_, shell::LaunchpadIndex::MatchMode::Prefix);
        return;
    }
    if (!current_folder_id_.empty()) {
        // A folder view is a projection of the index, so its item indices are only
        // valid until the index is rebuilt. Resolving by identity every time is
        // cheap against 208 items and keeps the mapping honest.
        for (const shell::LaunchpadFolderView& view : folder_views_) {
            if (view.id != current_folder_id_) continue;
            for (const shell::LaunchpadItem& item : view.items) {
                for (std::size_t i = 0; i < index_.Items().size(); ++i) {
                    if (index_.Items()[i].identity == item.identity) {
                        visible_.push_back(static_cast<int>(i));
                        break;
                    }
                }
            }
            break;
        }
        return;
    }
    // Flat grid: every item is visible.
    visible_.reserve(index_.Items().size());
    for (std::size_t i = 0; i < index_.Items().size(); ++i) {
        visible_.push_back(static_cast<int>(i));
    }
}

void Launchpad::RefreshFolderViews(const core::Config& config) {
    folder_views_ = index_.Folders(config.launchpad);
    UpdateFolderNames();
    // The open folder may have been deleted or renamed since the last frame; leaving
    // a stale id would show an empty grid with no way back.
    if (!current_folder_id_.empty()) {
        bool found = false;
        for (const shell::LaunchpadFolderView& view : folder_views_) {
            if (view.id == current_folder_id_) {
                found = true;
                break;
            }
        }
        if (!found) current_folder_id_.clear();
    }
    // Same for a folder mid-rename: if it vanished under the editor, the pending name
    // has nothing to commit to and must not survive to the next CommitRename.
    if (!caret_folder_id_.empty() && ChipIndexOf(caret_folder_id_) < 0) {
        caret_folder_id_.clear();
        rename_text_.clear();
    }
}

void Launchpad::UpdateFolderNames() {
    folder_names_.clear();
    folder_names_.reserve(folder_views_.size());
    for (const shell::LaunchpadFolderView& view : folder_views_) {
        // Names arrive as UTF-8 from the config; widening byte-wise would mangle
        // every non-ASCII folder name into mojibake on the chip.
        folder_names_.push_back(core::Utf8ToWide(view.name));
    }
}

int Launchpad::ChipIndexOf(const std::string& id) const {
    if (id.empty()) return -1;
    for (std::size_t i = 0; i < folder_views_.size(); ++i) {
        if (folder_views_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

int Launchpad::IconPxFor(const core::Config& config) const {
    double max_scale = 1.0;
    for (const auto& overlay : overlays_) {
        max_scale = std::max(max_scale, static_cast<double>(overlay->window.Scale()));
    }
    int icon_px = static_cast<int>(std::lround(static_cast<double>(config.launchpad.icon_size) *
                                               max_scale));
    return std::clamp(icon_px, 32, kMaxIconPx);
}

void Launchpad::ExtractTile(std::size_t index) {
    if (index >= tiles_.size()) return;
    LaunchpadTile& tile = tiles_[index];
    if (tile.attempted) return;
    tile.attempted = true;
    // tile_icon_px_ is what EnsureTiles built the prewarmed tiles at; lazy extraction
    // must ask for the same size or the two populations would render at mixed scales.
    if (!icons_->Get(shell::IconSourceFor(tile.app), tile_icon_px_, tile.icon)) {
        DF_DEBUG(kLogModule, "no icon for %s (lazy)", tile.identity.c_str());
    }
}

void Launchpad::EnterFolder(const std::string& id) {
    if (id.empty()) {
        LeaveFolder();
        return;
    }
    if (current_folder_id_ == id) return;
    current_folder_id_ = id;
    caret_folder_id_.clear();
    search_query_.clear();
    RefreshVisible();
    page_ = 0;
    ApplyPageClamp();
    const core::LaunchpadFolder* folder = dock::FindLaunchpadFolder(config_, id);
    DF_INFO(kLogModule, "folder opened id=%s name=%s items=%d", id.c_str(),
            folder ? folder->name.c_str() : "?", VisibleCount());
    Repaint();
}

void Launchpad::LeaveFolder() {
    if (current_folder_id_.empty()) return;
    current_folder_id_.clear();
    caret_folder_id_.clear();
    RefreshVisible();
    page_ = 0;
    ApplyPageClamp();
    DF_DEBUG(kLogModule, "folder closed; %d item(s)", VisibleCount());
    Repaint();
}

std::vector<int> Launchpad::PageIndices() const {
    std::vector<int> page;
    if (LaunchpadGrid grid = GridFor(0); grid.per_page > 0) {
        const int start = page_ * grid.per_page;
        const int count = std::min(grid.per_page, std::max(0, VisibleCount() - start));
        page.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            if (start + i < static_cast<int>(visible_.size())) {
                page.push_back(visible_[static_cast<std::size_t>(start + i)]);
            }
        }
    }
    return page;
}

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
    // The strip band is reserved whenever the config has folders, not only while one
    // is open: a grid that jumped up and down as the strip appeared and disappeared
    // would move the first row out from under the pointer mid-gesture.
    input.show_folder_strip = HasFolders();
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
        const int count = LaunchpadPageCount(VisibleCount(), grid);
        pages = (pages < 0) ? count : std::min(pages, count);
    }
    return std::max(1, pages < 0 ? 1 : pages);
}

void Launchpad::EnsureTiles(const core::Config& config) {
    if (!tiles_stale_ && !tiles_.empty()) return;

    // Extraction size follows the largest display scale so a 200% monitor does not get
    // an upscaled icon. `icon_size` is DIP; the shell source wants pixels. Kept in
    // tile_icon_px_ so lazy extraction asks for the same size (T-042).
    tile_icon_px_ = IconPxFor(config);

    const std::uint64_t start_ms = GetTickCount64();
    const std::vector<shell::LaunchpadItem>& items = index_.Items();
    // The prewarm covers the first two pages; the tail is prefilled with identity and
    // key only, and its icon is extracted on the first paint that shows it. A search
    // or a folder can surface any item, so "the grid only ever shows one page" no
    // longer bounds which tiles are needed.
    tiles_ = BuildLaunchpadTiles(items, *icons_, options_.max_icon_tiles, tile_icon_px_);
    const std::size_t prewarmed = tiles_.size();
    tiles_.resize(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i < prewarmed) {
            tiles_[i].attempted = true;  // the prewarm already paid this attempt
            continue;
        }
        LaunchpadTile& tile = tiles_[i];
        tile.app = items[i].app;
        tile.identity = items[i].identity;
        tile.icon_key = LaunchpadIconKey(tile.identity);
    }
    DF_INFO(kLogModule, "tiles built: %zu prewarmed of %zu items at %dpx in %.1fms",
            prewarmed, items.size(), tile_icon_px_,
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
    // Folder views are a projection of the index, so they must be resolved after the
    // rebuild. Doing it here (rather than on every read) keeps the chip strip and the
    // click handler in agreement about ordering.
    RefreshFolderViews(config);
    RefreshVisible();
    EnsureTiles(config);

    // A fresh open never resumes a gesture or an edit from the previous session; the
    // close transition already clears them, and this covers the first open after
    // startup where there was no close at all.
    press_active_ = false;
    press_cell_ = -1;
    press_identity_.clear();
    drag_identity_.clear();

    page_ = ClampLaunchpadPage(page_, VisibleCount(), GridFor(0));
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
            // The field state does not survive a full close: reopening with the last
            // session's query, folder or half-typed rename applied silently is more
            // surprising than starting clean (T-042).
            search_query_.clear();
            current_folder_id_.clear();
            caret_folder_id_.clear();
            rename_text_.clear();
            drag_identity_.clear();
            press_active_ = false;
            press_cell_ = -1;
            press_identity_.clear();
            RefreshVisible();
            DF_INFO(kLogModule, "closed");
        }
        dirty = true;
    }

    if (phase_ != LaunchpadPhase::Closed) {
        const int cells = LaunchpadPageItemCount(VisibleCount(), GridFor(0), page_);
        for (std::size_t i = 0; i < hovers_.size(); ++i) {
            // Only the display under the pointer can have an active cell; the rest decay.
            LaunchpadHover& hover = hovers_[i];
            if (static_cast<int>(i) != hover_display_) hover.active = false;
            if (AdvanceLaunchpadHover(hover, cells, animation_delta)) dirty = true;
        }

        // The caret blinks on a fixed cycle while the Launchpad is open. A keystroke
        // restarts the cycle from the visible half (Character/Backspace reset the
        // clock), so the caret never vanishes under a typing finger.
        caret_ms_ += animation_delta;
        const bool blink_on =
            std::fmod(caret_ms_, 2.0 * dock::kLaunchpadCaretBlinkMs) <
            static_cast<double>(dock::kLaunchpadCaretBlinkMs);
        if (blink_on != caret_visible_) {
            caret_visible_ = blink_on;
            dirty = true;
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
    const int clamped = ClampLaunchpadPage(page_, VisibleCount(), GridFor(0));
    if (clamped != page_) {
        DF_DEBUG(kLogModule, "page clamped %d -> %d", page_, clamped);
        page_ = clamped;
    }
}

void Launchpad::PaintOverlay(std::size_t display_index) {
    LaunchpadOverlay& overlay = *overlays_[display_index];
    if (!overlay.ready) return;

    const LaunchpadGrid grid = GridForOverlay(overlay, config_);
    const int items_on_page = LaunchpadPageItemCount(VisibleCount(), grid, page_);

    // The page slice of the projection, for this display's grid. Mixed-DPI displays
    // can disagree on per_page, so the slice is recomputed per display rather than
    // shared.
    paint_page_.clear();
    {
        const int start = page_ * grid.per_page;
        const int count = std::min(grid.per_page, std::max(0, VisibleCount() - start));
        paint_page_.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            const std::size_t position = static_cast<std::size_t>(start + i);
            if (position < visible_.size()) paint_page_.push_back(visible_[position]);
        }
    }
    // Tiles the prewarm did not cover are extracted here, once, the first time a cell
    // shows them. Cold extractions cost ~25ms each, so the first paint of a far page
    // can hitch once; the attempted flag keeps it to exactly once per tile.
    for (const int index : paint_page_) {
        if (index >= 0) ExtractTile(static_cast<std::size_t>(index));
    }

    LaunchpadPaintInput input;
    input.tiles = &tiles_;
    input.visible = &paint_page_;
    input.grid = grid;
    input.page = page_;
    input.page_count = LaunchpadPageCount(VisibleCount(), grid);
    input.items_on_page = items_on_page;
    input.width_dip = overlay.window.WidthDip();
    input.height_dip = overlay.window.HeightDip();
    input.dpi = static_cast<float>(overlay.window.Dpi());
    input.progress = progress_;
    input.hover = display_index < hovers_.size() ? &hovers_[display_index] : nullptr;
    input.selected = -1;

    // T-042 field state. The strip is drawn from the same folder_names_ order PressAt
    // hit-tests, and the open/rename markers are resolved to strip indexes here.
    input.query = &search_query_;
    input.caret_visible = caret_visible_;
    input.folder_names = &folder_names_;
    input.open_chip = ChipIndexOf(current_folder_id_);
    input.renaming_chip = ChipIndexOf(caret_folder_id_);
    input.rename_text = &rename_text_;
    input.rename_caret_visible = caret_visible_;

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

void Launchpad::PressAt(std::size_t display_index, float x_dip, float y_dip) {
    last_launched_ = nullptr;
    if (display_index >= overlays_.size()) return;
    if (phase_ == LaunchpadPhase::Closed || phase_ == LaunchpadPhase::Closing) return;

    LaunchpadOverlay& overlay = *overlays_[display_index];
    const float width_dip = overlay.window.WidthDip();

    // The folder strip sits above the grid. A press there enters (or leaves) the
    // folder and must not fall through to whatever cell happens to be under it.
    if (HasFolders()) {
        std::vector<render::RectF> chips;
        dock::LaunchpadFolderChipRects(width_dip, static_cast<int>(folder_names_.size()),
                                       chips);
        const int chip = dock::LaunchpadFolderChipHit(x_dip, y_dip, chips);
        if (chip >= 0 && chip < static_cast<int>(folder_views_.size())) {
            const std::string& id = folder_views_[static_cast<std::size_t>(chip)].id;
            // The chip of the open folder toggles back to the flat grid, the same way
            // the strip in every launcher doubles as the "back" control.
            if (current_folder_id_ == id) {
                LeaveFolder();
            } else {
                EnterFolder(id);
            }
            return;
        }
    }

    // The search field keeps the Launchpad open and just lights the caret; it is the
    // only other surface a press can land on without dismissing.
    if (dock::LaunchpadSearchHit(x_dip, y_dip, width_dip, overlay.window.HeightDip())) {
        caret_ms_ = 0.0;
        caret_visible_ = true;
        Repaint();
        return;
    }

    const LaunchpadGrid grid = GridForOverlay(overlay, config_);
    const int items_on_page = LaunchpadPageItemCount(VisibleCount(), grid, page_);
    const int cell = painter_.HitTest(grid, items_on_page, x_dip, y_dip);
    if (cell < 0) {
        // A click on the scrim (outside every cell) dismisses, the way every
        // full-screen launcher behaves. A click on the grid plate between cells also
        // lands here; the grid is one surface, and treating a near-miss as "stay open"
        // would feel stuck.
        Close();
        return;
    }

    // The cell index is relative to the page; `visible_` maps it back to the index.
    // While a folder is open or a query is active that list is a projection, so the
    // old page_ * per_page + cell arithmetic would launch the wrong app.
    press_active_ = true;
    press_display_ = display_index;
    press_cell_ = cell;
    press_x_ = x_dip;
    press_y_ = y_dip;
    press_identity_.clear();
    const std::vector<int> page_indices = PageIndices();
    if (cell < static_cast<int>(page_indices.size())) {
        const int item_index = page_indices[static_cast<std::size_t>(cell)];
        if (item_index >= 0 && item_index < static_cast<int>(index_.Items().size())) {
            press_identity_ = index_.Items()[static_cast<std::size_t>(item_index)].identity;
        }
    }
}

void Launchpad::ReleaseAt(std::size_t display_index, float x_dip, float y_dip) {
    const bool pressed = press_active_;
    press_active_ = false;
    if (!pressed || display_index != press_display_) return;
    if (phase_ == LaunchpadPhase::Closed || phase_ == LaunchpadPhase::Closing) return;
    if (press_identity_.empty()) return;

    if (DraggingItem()) {
        // A drop resolves against the cell under the pointer, not the pressed one.
        // Missing the grid entirely cancels: dropping an app on the scrim must not
        // create a folder, and it must not launch either (the press was a drag start).
        std::string target;
        LaunchpadOverlay& overlay = *overlays_[display_index];
        const LaunchpadGrid grid = GridForOverlay(overlay, config_);
        const int items_on_page = LaunchpadPageItemCount(VisibleCount(), grid, page_);
        const int cell = painter_.HitTest(grid, items_on_page, x_dip, y_dip);
        if (cell >= 0) {
            const std::vector<int> page_indices = PageIndices();
            if (cell < static_cast<int>(page_indices.size())) {
                const int item_index = page_indices[static_cast<std::size_t>(cell)];
                if (item_index >= 0 && item_index < static_cast<int>(index_.Items().size())) {
                    target = index_.Items()[static_cast<std::size_t>(item_index)].identity;
                }
            }
        }
        EndItemDrag(target);
        return;
    }

    // Plain click: the pressed cell launches, wherever the pointer drifted before the
    // release. Launching on release (not on press) is what leaves room for the drag
    // gesture to exist at all (T-042).
    const std::vector<int> page_indices = PageIndices();
    if (press_cell_ < 0 || press_cell_ >= static_cast<int>(page_indices.size())) return;
    const int item_index = page_indices[static_cast<std::size_t>(press_cell_)];
    if (item_index < 0 || item_index >= static_cast<int>(index_.Items().size())) return;
    const shell::LaunchpadItem& item = index_.Items()[static_cast<std::size_t>(item_index)];

    const shell::LaunchResult result = shell::Launch(item.app);
    DF_INFO(kLogModule, "launch %s -> %s route=%s pid=%u",
            core::WideToUtf8(item.app.display_name).c_str(), ToString(result.error),
            result.route, result.process_id);
    last_launched_ = &item;
    Close();
}

const shell::LaunchpadItem* Launchpad::ClickAt(std::size_t display_index, float x_dip,
                                               float y_dip) {
    PressAt(display_index, x_dip, y_dip);
    ReleaseAt(display_index, x_dip, y_dip);
    return last_launched_;
}

void Launchpad::PointerMoved(std::size_t display_index, float x_dip, float y_dip) {
    if (display_index >= overlays_.size() || phase_ == LaunchpadPhase::Closed) return;
    hover_display_ = static_cast<int>(display_index);

    const LaunchpadGrid grid = GridForOverlay(*overlays_[display_index], config_);
    const int items_on_page = LaunchpadPageItemCount(VisibleCount(), grid, page_);
    const int cell = painter_.HitTest(grid, items_on_page, x_dip, y_dip);

    // A press that travels past the threshold while still down becomes a drag. The
    // distance gate is what separates it from a sloppy click; the identity is fixed at
    // press time, so a drag that starts on a cell keeps carrying that app even if the
    // pointer leaves its cell for a moment.
    if (press_active_ && display_index == press_display_ && !press_identity_.empty() &&
        !DraggingItem()) {
        const float dx = x_dip - press_x_;
        const float dy = y_dip - press_y_;
        if (dx * dx + dy * dy > kDragThresholdDip * kDragThresholdDip) {
            BeginItemDrag(press_identity_);
            DF_DEBUG(kLogModule, "drag started from %s", press_identity_.c_str());
        }
    }

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
    // Diagnostic for the probe channel: a `key` command that produces no visible
    // effect is indistinguishable from one that never arrived without this line.
    DF_DEBUG(kLogModule, "key vk=%u renaming=%d folder_open=%d", vk, Renaming() ? 1 : 0,
             current_folder_id_.empty() ? 0 : 1);
    // The overlay routes the real Escape through `dismissed`, never through here; the
    // probe channel goes the other way. Handling it in both places keeps one unwind.
    if (vk == VK_ESCAPE) {
        Escape();
        return;
    }
    if (vk == VK_BACK) {
        Backspace();
        return;
    }
    if (Renaming()) {
        // While the rename editor is up the keyboard belongs to it: paging or moving
        // the grid under an active editor would leave the caret pointing at a chip
        // the user can no longer see.
        if (vk == VK_RETURN) CommitRename();
        return;
    }
    if (vk == VK_F2) {
        // Rename applies to the open folder; F2 on the flat grid has no target.
        if (!current_folder_id_.empty()) BeginRename(current_folder_id_);
        return;
    }
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

void Launchpad::Character(wchar_t ch) {
    caret_ms_ = 0.0;
    caret_visible_ = true;
    // Control characters never belong in a query or a name: WM_CHAR also delivers a
    // second copy of Enter ('\r') and Backspace ('\b') that WM_KEYDOWN already
    // reported, and a stray Tab typed during the open animation would otherwise be
    // filtered into the field.
    if (ch < 0x20) return;
    if (!caret_folder_id_.empty()) {
        rename_text_.push_back(ch);
        Repaint();
        return;
    }
    search_query_.push_back(ch);
    current_folder_id_.clear();
    RefreshVisible();
    page_ = 0;
    ApplyPageClamp();
    DF_DEBUG(kLogModule, "search '%s' -> %d item(s)", core::WideToUtf8(search_query_).c_str(),
             VisibleCount());
    Repaint();
}

void Launchpad::Backspace() {
    caret_ms_ = 0.0;
    caret_visible_ = true;
    if (!caret_folder_id_.empty()) {
        if (!rename_text_.empty()) rename_text_.pop_back();
        Repaint();
        return;
    }
    if (search_query_.empty()) return;
    search_query_.pop_back();
    // Deleting the last character clears the folder context too: the same rule as
    // SetSearchQuery, so leaving a folder and typing a query behave identically.
    if (search_query_.empty()) current_folder_id_.clear();
    RefreshVisible();
    page_ = 0;
    ApplyPageClamp();
    Repaint();
}

void Launchpad::ClearField() {
    if (Renaming()) {
        rename_text_.clear();
        caret_ms_ = 0.0;
        caret_visible_ = true;
        Repaint();
        return;
    }
    SetSearchQuery(L"");
}

void Launchpad::BeginRename(const std::string& id) {
    const core::LaunchpadFolder* folder = dock::FindLaunchpadFolder(config_, id);
    if (folder == nullptr) {
        DF_WARN(kLogModule, "rename requested for unknown folder id '%s'", id.c_str());
        return;
    }
    caret_folder_id_ = id;
    rename_text_ = core::Utf8ToWide(folder->name);
    caret_ms_ = 0.0;
    caret_visible_ = true;
    DF_INFO(kLogModule, "rename started id=%s name=%s", id.c_str(), folder->name.c_str());
    Repaint();
}

void Launchpad::CommitRename() {
    if (caret_folder_id_.empty()) return;
    // An empty commit would rename the folder to a blank chip the user cannot
    // identify; C-18 falls back to the id for display, which is noise, not a name.
    // Keeping the old name (cancelling) is the honest outcome.
    if (rename_text_.empty()) {
        DF_WARN(kLogModule, "rename of '%s' committed an empty name; keeping the old one",
                caret_folder_id_.c_str());
        CancelRename();
        return;
    }
    // dock::RenameLaunchpadFolder keeps the id stable, which is the whole reason the
    // rename path exists in launchpad_folders rather than here.
    const bool renamed = dock::RenameLaunchpadFolder(config_, caret_folder_id_,
                                                     core::WideToUtf8(rename_text_));
    if (renamed) {
        const bool saved = dock::SaveLaunchpadConfig(config_);
        DF_INFO(kLogModule, "folder renamed id=%s saved=%d", caret_folder_id_.c_str(),
                saved ? 1 : 0);
    } else {
        DF_WARN(kLogModule, "rename refused id=%s", caret_folder_id_.c_str());
    }
    caret_folder_id_.clear();
    rename_text_.clear();
    RefreshFolderViews(config_);
    RefreshVisible();
    Repaint();
}

void Launchpad::Escape() {
    if (Renaming()) {
        CancelRename();
    } else if (!current_folder_id_.empty()) {
        LeaveFolder();
    } else {
        Close();
    }
}

void Launchpad::CancelRename() {
    if (caret_folder_id_.empty()) return;
    DF_INFO(kLogModule, "rename cancelled id=%s", caret_folder_id_.c_str());
    caret_folder_id_.clear();
    rename_text_.clear();
    Repaint();
}

void Launchpad::BeginItemDrag(const std::string& identity) {
    drag_identity_ = identity;
}

void Launchpad::EndItemDrag(const std::string& target_identity) {
    if (drag_identity_.empty()) return;
    const std::string source = drag_identity_;
    drag_identity_.clear();
    if (target_identity.empty() || target_identity == source) return;
    CreateFolderFromDrop(source, target_identity);
}

std::string Launchpad::CreateFolderFromDrop(const std::string& identity,
                                            const std::string& target_identity) {
    if (identity.empty() || target_identity.empty() || identity == target_identity) {
        return std::string();
    }
    // A folder that already holds either identity would gain a duplicate, which
    // AppendToLaunchpadFolder refuses silently - so look for an existing folder
    // first and append to it instead of creating a second one.
    for (const core::LaunchpadFolder& folder : config_.launchpad.folders) {
        const bool holds_identity =
            dock::LaunchpadFolderContains(config_, folder.id, identity);
        const bool holds_target =
            dock::LaunchpadFolderContains(config_, folder.id, target_identity);
        if (holds_identity && holds_target) return std::string();
        if (holds_identity || holds_target) {
            const std::string& missing = holds_identity ? target_identity : identity;
            dock::AppendToLaunchpadFolder(config_, folder.id, missing);
            const bool saved = dock::SaveLaunchpadConfig(config_);
            DF_INFO(kLogModule, "folder %s gained %s saved=%d", folder.id.c_str(),
                    core::WideToUtf8(std::wstring(missing.begin(), missing.end())).c_str(),
                    saved ? 1 : 0);
            RefreshFolderViews(config_);
            RefreshVisible();
            Repaint();
            return folder.id;
        }
    }
    const std::string id = dock::CreateLaunchpadFolder(config_, "New Folder",
                                                       {identity, target_identity});
    if (id.empty()) {
        DF_WARN(kLogModule, "folder create refused for %s + %s",
                core::WideToUtf8(std::wstring(identity.begin(), identity.end())).c_str(),
                core::WideToUtf8(std::wstring(target_identity.begin(), target_identity.end()))
                    .c_str());
        return std::string();
    }
    const bool saved = dock::SaveLaunchpadConfig(config_);
    DF_INFO(kLogModule, "folder created id=%s items=2 saved=%d", id.c_str(), saved ? 1 : 0);
    RefreshFolderViews(config_);
    RefreshVisible();
    Repaint();
    return id;
}

}  // namespace dockforge::dock
