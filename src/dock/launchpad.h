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
    // Caps the number of icons resolved per open. One page is all that is ever drawn,
    // and 208 entries at 256px would be ~54MB of pixels for a grid that shows 35.
    int max_icon_tiles = 70;  // two pages of headroom for a fast page flip
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

    // Routes a click at (x_dip, y_dip) on `display_index`. Returns the entry that was
    // launched, or nullptr. The Launchpad closes itself after a successful launch.
    const shell::LaunchpadItem* ClickAt(std::size_t display_index, float x_dip, float y_dip);

    // Pointer state, per display. Only the display the pointer is on has an active
    // hover; the others keep their eased amounts decaying to zero.
    void PointerMoved(std::size_t display_index, float x_dip, float y_dip);
    void PointerLeft(std::size_t display_index);

    // Keyboard: Escape closes, Left/Right/PageUp/PageDown page, Home/End jump.
    void KeyPressed(unsigned int vk);

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

    // Last page handed to the painter, so the per-frame debug line only appears when the
    // page actually changes instead of 60 times a second.
    int painted_page_ = -1;
};

}  // namespace dockforge::dock
