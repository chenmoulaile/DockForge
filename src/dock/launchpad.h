// DockForge - Launchpad controller: one overlay per display, one shared index.
//
// Responsibilities, and why they are grouped this way:
//
//   * owns the overlays (one per display, ADR-0021) and their composition surfaces;
//   * owns the shared shell::LaunchpadIndex and the tile list derived from it;
//   * owns the per-frame state that has to agree across displays: the current page,
//     which is one number for the whole Launchpad, and the animation progress, which
//     is also global so a two-display Launchpad opens as one gesture.
//
// It does NOT own the app loop. The caller drives it from the existing tick handler,
// so the Dock's frame cadence stays the single clock (C-12: one frame source).
//
// Why the index is rebuilt off the frame path: measured at 360-1045ms on this machine
// (evidence/T-040). Rebuilding inside Tick would freeze the Dock for that long. The
// controller therefore rebuilds on the first Open() only, and from then on relies on
// the watcher's settled-change signal.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "dock/launchpad_layout.h"
#include "dock/launchpad_search.h"
#include "dock/launchpad_view.h"
#include "platform/composition.h"
#include "platform/display.h"
#include "platform/overlay_window.h"
#include "render/backend.h"
#include "render/material.h"
#include "shell/icon_cache.h"
#include "shell/launchpad_index.h"

namespace dockforge::dock {

// One display's overlay plus its composition surface.
//
// Held by unique_ptr in Launchpad: OverlayWindow and CompositionSurface are both
// non-copyable and non-movable (they own a HWND and a DComp visual bound to it), so a
// by-value vector would need to move them and the compiler deletes the whole type.
// Measured: a by-value vector fails to compile with "use of deleted function
// LaunchpadOverlay::LaunchpadOverlay(LaunchpadOverlay&&)".
struct LaunchpadOverlay {
    platform::OverlayWindow window;
    platform::CompositionSurface surface;
    render::PlateRenderer plate;
    std::unique_ptr<render::IRenderBackend> backend;
    platform::DisplayInfo display;
    bool ready = false;
};

// Why the Launchpad is currently open, for the log and for the dismissal rules.
enum class LaunchpadPhase {
    Closed,
    Opening,
    Open,
    Closing,
};

const char* ToString(LaunchpadPhase phase);

struct LaunchpadOptions {
    // How many icons are extracted up front when the tiles are built. A search or an
    // open folder can surface any item of the index, so tiles beyond this budget are
    // extracted lazily on the first paint that needs them (T-042) - one page of cold
    // extractions is ~900ms once, while extracting all 208 up front is ~5s on every
    // cold start. Two pages of headroom keeps paging and the first searches warm.
    int max_icon_tiles = 70;
    int icon_px = 128;
};

class Launchpad {
  public:
    Launchpad();
    ~Launchpad();

    Launchpad(const Launchpad&) = delete;
    Launchpad& operator=(const Launchpad&) = delete;

    // Builds the overlays for every display. Called once at startup; a failure on one
    // display is logged and that display is skipped rather than aborting the app,
    // because a broken secondary monitor must not take the Dock down with it.
    bool Initialize(HINSTANCE instance, const core::Config& config, shell::IconCache& icons,
                    const LaunchpadOptions& options);

    // Rebuilds the item index from `config`. Blocking: 360-1045ms measured. Called from
    // Open() when the index is empty, and from the tick when the watcher reports a
    // settled change while closed.
    void RebuildIndex(const core::Config& config);

    bool IndexEmpty() const { return index_.Empty(); }
    int ItemCount() const;

    // Releases the overlays and their composition surfaces. Called before the logger
    // shuts down so the teardown itself is still logged; the destructor repeats it
    // idempotently for the early-return paths.
    void Shutdown();

    // Extracts the icons the grid will need, so the first Open() does not pay for it.
    // Measured: 70 icons at 192px cost 1797ms, which is far too long to spend inside the
    // frame that answers the open gesture. Safe to call before any overlay is shown;
    // calling it again after a rebuild is what keeps the icons in step with the index.
    void WarmTiles(const core::Config& config);

    // Shows every overlay and starts the open animation. The index is rebuilt first if
    // it has never been built.
    void Open(const core::Config& config);
    void Close();

    // Drives the animation and any pending rebuild. Returns true when the frame needs
    // repainting, which is what the Dock's tick uses to decide on InvalidateRect.
    bool Tick(double delta_ms, const core::Config& config);

    // True while any overlay is visible.
    bool Visible() const { return phase_ != LaunchpadPhase::Closed; }
    LaunchpadPhase Phase() const { return phase_; }
    int Page() const { return page_; }
    int PageCount() const;

    // Mouse gesture (T-042). A press records the cell under the pointer and shows the
    // field caret; movement past the drag threshold turns the gesture into a drag; the
    // release either drops (drag) or launches the pressed cell (click). Chip, search
    // field and scrim are decided at press time, like every full-screen launcher.
    void PressAt(std::size_t display_index, float x_dip, float y_dip);
    void ReleaseAt(std::size_t display_index, float x_dip, float y_dip);

    // A complete press+release at one point, which is what a click is. Returns the
    // entry that was launched, or nullptr. Kept as one call because the probe channel
    // (main.cpp) injects clicks, not gestures - the real mouse path goes through
    // PressAt/ReleaseAt via the overlay's WM_LBUTTONDOWN/WM_LBUTTONUP.
    const shell::LaunchpadItem* ClickAt(std::size_t display_index, float x_dip, float y_dip);

    // Types `text` into the search field, replacing the current query. An empty string
    // clears the filter, which is what Backspace down to nothing should do.
    void SetSearchQuery(const std::wstring& text);
    const std::wstring& SearchQuery() const { return search_query_; }
    // How many items the current query keeps. Reported so the empty state can be drawn
    // without the view reaching into the index.
    int VisibleCount() const;
    bool SearchActive() const { return !search_query_.empty(); }

    // Folder navigation: the strip is only drawn when the config has folders.
    bool HasFolders() const;
    // Enters a folder by id, or -1 to return to the flat grid. Rebuilds the visible
    // list from the index on entry so a folder added while closed appears immediately.
    void EnterFolder(const std::string& id);
    void LeaveFolder();
    const std::string& CurrentFolderId() const { return current_folder_id_; }

    // Pointer state, per display. Only the display the pointer is on has an active
    // hover; the others keep their eased amounts decaying to zero.
    void PointerMoved(std::size_t display_index, float x_dip, float y_dip);
    void PointerLeft(std::size_t display_index);

    // Keyboard: Escape closes, Left/Right/PageUp/PageDown page, Home/End jump.
    void KeyPressed(unsigned int vk);
    // Escape unwinds one level at a time: a rename first, then an open folder, then
    // the Launchpad itself. Split out of the dismissed callback so the probe channel
    // can drive the same unwind the real keyboard takes.
    void Escape();
    // Typed characters go to the search field while it has focus, or to the rename
    // field while a folder is being renamed (T-042). Anything else is ignored.
    void Character(wchar_t ch);
    // Backspace in the search field removes one character; in the rename field it
    // edits the pending name. Enter commits a rename, Escape cancels it.
    void Backspace();
    // Wipes the active field in one gesture: the whole pending name while renaming,
    // the whole query otherwise. The keyboard path to it is select-all + delete;
    // the probe channel uses it because a command file cannot send N identical
    // backspaces (the channel drops a command equal to the previous one).
    void ClearField();
    void CommitRename();
    void CancelRename();
    // Begins renaming the folder under the pointer, or by id. The field is drawn over
    // the chip with the current name pre-selected.
    void BeginRename(const std::string& id);
    bool Renaming() const { return !caret_folder_id_.empty(); }
    const std::string& RenamingFolderId() const { return caret_folder_id_; }

    // Drag-to-create (T-042 DoD 3): dragging app `identity` onto the cell holding
    // `target_identity` creates a folder holding both and persists it. Returns the
    // new folder id, or an empty string when nothing was written.
    std::string CreateFolderFromDrop(const std::string& identity,
                                     const std::string& target_identity);
    // Drag bookkeeping, so a drag that ends on the scrim is not mistaken for a
    // folder creation.
    void BeginItemDrag(const std::string& identity);
    void EndItemDrag(const std::string& target_identity);
    bool DraggingItem() const { return !drag_identity_.empty(); }
    const std::string& DragIdentity() const { return drag_identity_; }

    const shell::LaunchpadIndexStats& Stats() const { return index_.Stats(); }
    std::size_t OverlayCount() const { return overlays_.size(); }
    const LaunchpadOverlay* OverlayAt(std::size_t display_index) const;

    // Exposed for the probe: the grid actually used by a display, so a test can
    // predict cell positions from the same code the paint path uses.
    LaunchpadGrid GridFor(std::size_t display_index) const;

    // Repaints every visible overlay once. Exposed because the caller owns the message
    // pump: nothing repaints unless it asks.
    void Repaint();

  private:
    void PaintOverlay(std::size_t display_index);
    void ApplyPageClamp();
    void EnsureTiles(const core::Config& config);
    void DestroyOverlays();
    LaunchpadGrid GridForOverlay(const LaunchpadOverlay& overlay, const core::Config& config) const;

    // Recomputes `visible_` (indices into index_.Items()) from the search query and
    // the open folder. Called on every query change and folder transition, never per
    // frame: the index is small (208 items) but the paging arithmetic runs per frame.
    void RefreshVisible();
    // The items the current page should draw, as indices into index_.Items().
    std::vector<int> PageIndices() const;
    // Resolves config.launchpad.folders against the index. Cheap enough to call on
    // demand; cached here so the chip strip and the click handler agree on ordering.
    void RefreshFolderViews(const core::Config& config);
    // Rebuilds the strip labels from `folder_views_` (wide strings for the painter).
    // Kept beside RefreshFolderViews because both must agree on order and existence.
    void UpdateFolderNames();
    // Index of a folder id in the strip, or -1. The strip is what the painter draws
    // and what PressAt hit-tests, so open/rename states are expressed in strip space.
    int ChipIndexOf(const std::string& id) const;
    // Extraction size for tiles, following the largest display's scale.
    int IconPxFor(const core::Config& config) const;
    // Resolves one tile's icon on first need. Safe to call repeatedly: the attempted
    // flag makes every call after the first a no-op, including tiles the shell
    // genuinely cannot provide an icon for.
    void ExtractTile(std::size_t index);

    HINSTANCE instance_ = nullptr;
    LaunchpadOptions options_{};
    core::Config config_{};  // snapshot; the Launchpad never edits config
    shell::IconCache* icons_ = nullptr;

    std::vector<std::unique_ptr<LaunchpadOverlay>> overlays_;
    LaunchpadPainter painter_;
    shell::LaunchpadIndex index_;
    shell::LaunchpadWatcher watcher_;
    std::vector<LaunchpadTile> tiles_;
    bool tiles_stale_ = true;

    // Search state (T-042). The query is the source of truth for `visible_`; the
    // field itself only forwards characters.
    std::wstring search_query_;
    std::vector<int> visible_;

    // Folder state (T-042). `current_folder_id_` empty means the flat grid. The views
    // are a projection of config.launchpad.folders against the index, refreshed when
    // the config or the index changes; `folder_names_` is the wide-string strip the
    // painter draws, in the same order.
    std::vector<shell::LaunchpadFolderView> folder_views_;
    std::vector<std::wstring> folder_names_;
    std::string current_folder_id_;
    std::string caret_folder_id_;  // folder pending rename, "" when not renaming

    LaunchpadPhase phase_ = LaunchpadPhase::Closed;
    float progress_ = 0.0f;
    int page_ = 0;
    std::vector<LaunchpadHover> hovers_;  // one per overlay
    int hover_display_ = -1;

    // Set when the watcher reported a settled change while closed. The rebuild itself
    // is deferred to the next Open(): doing it inside Tick would block the dock's frame
    // loop for 360-1045ms (evidence/T-040), which is a visible freeze on a surface the
    // user is not even looking at yet.
    bool index_stale_ = false;

    // Caret blink state for the search field (T-042). The field only draws the caret
    // when the elapsed time since the last keystroke is inside the visible half of the
    // blink cycle, so a query left alone does not pulse forever.
    double caret_ms_ = 0.0;
    bool caret_visible_ = false;

    // Drag-to-create state (T-042). A press records the cell and its identity; moving
    // past the drag threshold promotes the gesture to a drag (`drag_identity_` set);
    // the release either drops or launches. `rename_text_` is the folder name being
    // edited, valid only while `caret_folder_id_` is set.
    bool press_active_ = false;
    std::size_t press_display_ = 0;
    int press_cell_ = -1;
    std::string press_identity_;
    float press_x_ = 0.0f;
    float press_y_ = 0.0f;
    std::string drag_identity_;
    std::wstring rename_text_;
    // The entry launched by the most recent complete gesture, returned by ClickAt.
    const shell::LaunchpadItem* last_launched_ = nullptr;

    // Scratch for the painter: the page slice of `visible_` for the display being
    // painted. A member so the per-frame allocation does not churn the heap.
    std::vector<int> paint_page_;
    // Extraction size the current tiles were built for; lazy extraction must ask for
    // the same size or the shell returns a differently scaled bitmap.
    int tile_icon_px_ = 96;

    // Last page handed to the painter, so the per-frame debug line only appears when the
    // page actually changes instead of 60 times a second.
    int painted_page_ = -1;
};

}  // namespace dockforge::dock
