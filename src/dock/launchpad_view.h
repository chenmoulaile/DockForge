// DockForge - Launchpad grid view: tiles, hover, paging and painting (C-18).
//
// Split from the window on purpose. The window owns a HWND, a composition surface and
// a message hook; this owns what is on screen. That keeps the paint path testable in
// the same way dock_view is: given the same items, page and hover, it must draw the
// same frame, and none of that needs a real display.
//
// Icons are resolved once at build time, not per frame. A Launchpad with 208 entries
// that re-extracted an icon per frame would spend 60-190ms per icon (evidence/T-006)
// and never reach a frame.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dock/launchpad_layout.h"
#include "render/backend.h"
#include "shell/app_entry.h"
#include "shell/icon_cache.h"
#include "shell/launchpad_index.h"

namespace dockforge::dock {

// One painted cell.
struct LaunchpadTile {
    shell::AppEntry app;
    shell::IconBitmap icon;
    std::uint64_t icon_key = 0;
    std::string identity;  // C-15a identity; what launchpad.folders[].items refers to
    // True once the shell was asked for this tile's icon, successfully or not. The
    // tail of a large index is extracted lazily on first paint (T-042): a search or
    // a folder can surface any item, and extracting all 208 up front would cost
    // ~5s on a cold cache at startup. The flag keeps a tile whose icon the shell
    // genuinely cannot provide from being re-requested every frame.
    bool attempted = false;
};

// Stable per-tile bitmap identity for the render backend.
//
// FNV-1a over the identity string, not over the pixels: the backend keys its GPU
// bitmap on this value and re-uploads when the size changes, so it must be stable
// across frames for the same app and different for different apps. Two apps whose
// hashes collide would swap artwork, which is why the hash is 64-bit rather than the
// 16-bit one the dock's placeholder key used.
std::uint64_t LaunchpadIconKey(const std::string& identity);

// Resolves icons for every item, in order. `max_tiles` bounds the work: an index of
// 208 entries at 256px each is ~54MB of pixels, so the caller passes the page size
// ceiling rather than the whole list when it only intends to show one page.
std::vector<LaunchpadTile> BuildLaunchpadTiles(const std::vector<shell::LaunchpadItem>& items,
                                               shell::IconCache& icons, int max_tiles,
                                               int icon_px);

// Hover state for the grid. One amount per visible cell so a fast pointer sweep does
// not snap the previous cell's icon back to full size in one frame.
struct LaunchpadHover {
    bool active = false;
    int index_on_page = -1;
    std::vector<float> amounts;  // eased 0..1 per cell
};

// Advances every cell's hover amount toward its target. Returns true when anything
// moved, so the caller can skip the redraw when the grid is at rest.
bool AdvanceLaunchpadHover(LaunchpadHover& hover, int cells, double delta_ms);

struct LaunchpadPaintInput {
    const std::vector<LaunchpadTile>* tiles = nullptr;
    // The tiles actually on screen, as indices into `tiles`. Non-empty when a search
    // query or an open folder projects the index; the painter resolves each index
    // itself so it never assumes page * per_page.
    const std::vector<int>* visible = nullptr;
    LaunchpadGrid grid;
    int page = 0;
    int page_count = 1;
    int items_on_page = 0;
    float width_dip = 0.0f;
    float height_dip = 0.0f;
    float dpi = 96.0f;
    // 0 = first frame of the open animation, 1 = fully open.
    float progress = 1.0f;
    const LaunchpadHover* hover = nullptr;
    // Optional: the app the pointer is over, drawn with a selection plate. Kept
    // separate from `hover` so the animation state and the paint stay decoupled.
    int selected = -1;

    // T-042 search field. `query` is the current text; `caret_visible` gates the
    // caret so it blinks rather than sitting lit all the time.
    const std::wstring* query = nullptr;
    bool caret_visible = false;

    // T-042 folder strip. `folder_names` is the strip order (the index sorts folders
    // by name), already clipped by the chip layout to what fits; `open_chip` and
    // `renaming_chip` are indexes into that same list, -1 when the grid is flat or
    // nothing is being renamed. The painter cannot map an id to a chip itself, so the
    // controller resolves the indexes once per frame.
    const std::vector<std::wstring>* folder_names = nullptr;
    int open_chip = -1;
    int renaming_chip = -1;
    // The pending name while `renaming_chip` is set; the editor draws it over the
    // chip's label with a caret.
    const std::wstring* rename_text = nullptr;
    bool rename_caret_visible = false;
};

// Draws one frame. Returns false only when the caller handed over no tiles.
class LaunchpadPainter {
  public:
    bool Paint(ID2D1DeviceContext& dc, render::IRenderBackend& backend, const LaunchpadPaintInput& input);

    // Same hit test the window uses, in DIP. Exposed so the window never re-derives
    // cell geometry from raw pixels.
    int HitTest(const LaunchpadGrid& grid, int items_on_page, float x_dip, float y_dip) const;

  private:
    void PaintBackdrop(render::IRenderBackend& backend, const LaunchpadPaintInput& input);
    void PaintCells(render::IRenderBackend& backend, const LaunchpadPaintInput& input);
    void PaintDots(render::IRenderBackend& backend, const LaunchpadPaintInput& input);
    void PaintSearchField(render::IRenderBackend& backend, const LaunchpadPaintInput& input);
    void PaintFolderStrip(render::IRenderBackend& backend, const LaunchpadPaintInput& input);
    void PaintEmptyState(render::IRenderBackend& backend, const LaunchpadPaintInput& input);
};

}  // namespace dockforge::dock
