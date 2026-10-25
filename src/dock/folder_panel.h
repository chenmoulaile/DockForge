// DockForge - folder drill-through panel above the Dock (T-066, ADR-0023).
//
// A lightweight in-Dock file browser: left-clicking a folder tile pops this
// panel instead of launching Explorer, clicking a subdirectory drills inside
// the panel, Esc / click-outside close it. Enumeration is IShellFolder (the
// logic layer in src/shell/folder_browser.*); drag-out is the shell's own
// CF_HDROP data object; content refresh is LaunchpadWatcher (the T-040
// ReadDirectoryChangesW + 500ms debounce pattern, reused rather than reinvented).
//
// GDI on purpose: the panel is small, appears on a click, and must not drag the
// D2D device or the composition tree into a failure path the Dock has to
// survive. Same trade the T-015 preview popup made.
#pragma once

#include "platform/win_common.h"

#include <map>
#include <string>
#include <vector>

#include "shell/folder_browser.h"
#include "shell/launchpad_index.h"

namespace dockforge::dock {

class FolderPanel {
  public:
    FolderPanel() = default;
    ~FolderPanel();

    FolderPanel(const FolderPanel&) = delete;
    FolderPanel& operator=(const FolderPanel&) = delete;

    // Registers the popup window class once per process (PreviewWindow's rule:
    // a registration failure is a startup problem, reported once).
    static bool RegisterWindowClass(HINSTANCE instance);

    // Opens the panel rooted at `folder_path`, horizontally centred on
    // `anchor_center_px` and sitting just above `dock_top_px` (both physical
    // screen pixels). Re-calls with the same root re-root (not toggle - the
    // toggle lives with the caller, which knows the tile). Returns false when
    // the window could not be created.
    bool Open(HINSTANCE instance, const std::wstring& folder_path, int anchor_center_px,
              int dock_top_px, float dpi);

    // Hides the window. The HWND survives so the next open does not pay for
    // window creation; the watcher stops so a closed panel watches nothing.
    void Close();

    bool Visible() const;
    bool IsValid() const { return hwnd_ != nullptr; }

    const std::wstring& RootFolder() const { return nav_.Root(); }
    const std::wstring& CurrentFolder() const { return nav_.Current(); }
    // The status of the last enumeration of the current level, for the log and
    // the placeholder decision.
    shell::FolderStatus Status() const { return status_; }

    // Navigation. DrillInto matches by full path (clicks) and returns false
    // when the path is not a navigable folder level. Back pops one level and
    // returns false at the root.
    bool DrillInto(const std::wstring& full_path);
    bool Back();

    // Esc semantics, shared by the physical WM_KEYDOWN path and the probe
    // command so a PASS proves the same chain: unwind one level at a time,
    // close at the root (the T-042 policy).
    void Escape();

    // Re-enumerates the current level. Called on drill, on back, and by the
    // watcher - never per frame.
    void ForceRefresh();

    // Tick support: starts/stops the ReadDirectoryChangesW watcher for the
    // current level and consumes its settled (500ms debounce) signal. Call
    // every tick while visible; returns true when the content changed and the
    // panel repainted itself. The clock is GetTickCount64, the same one the
    // Launchpad watcher consumer uses.
    bool TickWatcher();

    // ---- probe / test observability ------------------------------------
    int RowCount() const { return static_cast<int>(rows_.size()); }
    // Display name of row `index`, or an empty string. Rows include the
    // leading ".." back row when not at the root.
    std::wstring RowName(int index) const;
    // The row the pointer is over, or -1.
    int HoveredRow() const { return hovered_row_; }
    // Probe/test: leaves the hover state a real WM_MOUSEMOVE over row `index`
    // would leave (the hint strip and row highlight read it). -1 clears. An
    // out-of-range index is clamped to -1, never wrapped.
    void SetHoveredRowForProbe(int row);

  private:
    struct Row {
        std::wstring name;      // display name (".." for the back row)
        std::wstring full_path; // absolute; empty for the back row
        bool is_folder = false;
        bool is_back = false;
    };

    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    // One enumeration + icon eviction + repaint. `reason` lands in the log.
    void Reload(const char* reason);
    void LayoutAndShow();
    // Physical pixel geometry derived from the DIP layout at the open DPI.
    int RowHeightPx() const;
    int HeaderHeightPx() const;
    int HintHeightPx() const;
    // Hint strip layout for the currently hovered row. File names contain no
    // spaces, so DT_WORDBREAK cannot wrap them; the wrap is done by character
    // count instead, up to kHintMaxLines, with an ellipsis only on a still-
    // overflowing last line. Empty when nothing is hovered.
    void UpdateHintParts();
    // Grows/shrinks the window for the hover hint, keeping the bottom edge
    // (the panel must keep sitting on the dock).
    void RelayoutForHover();
    int hint_lines_ = 0;
    std::vector<std::wstring> hint_parts_;
    int hint_line_px_ = 0;  // font line height, physical px, set at open
    // Row under a client point, or -1. Accounts for scroll and the back row.
    int HitRow(int x_px, int y_px) const;
    void ActivateRow(int row);
    // OLE drag-out: the shell data object (CF_HDROP) for one row + DoDragDrop.
    void StartDragOut(int row);
    void Paint(HDC dc);
    HICON IconFor(const Row& row);

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    float dpi_ = 96.0f;
    float scale_ = 1.0f;

    shell::FolderNavStack nav_{L""};
    shell::FolderStatus status_ = shell::FolderStatus::Ok;
    std::vector<Row> rows_;
    // Icons resolved lazily per path and dropped on every reload: a stale icon
    // for a renamed file is worse than a re-resolve (SHGetFileInfo hits the
    // system image list, which is warm).
    std::map<std::wstring, HICON> icons_;

    // Geometry, physical pixels, recomputed on open and on DPI change.
    int width_px_ = 0;
    int height_px_ = 0;
    int anchor_center_px_ = 0;
    int dock_top_px_ = 0;
    int visible_rows_ = 10;
    int scroll_row_ = 0;

    int hovered_row_ = -1;
    // True once the panel has held activation at least once. WM_ACTIVATE
    // arrival order vs the foreground grant is not guaranteed, so a transient
    // WA_INACTIVE before any activation is not treated as a dismissal.
    bool was_activated_ = false;
    // Set while a TrackMouseEvent hover is armed; WM_MOUSELEAVE clears the
    // hovered row so the hint strip does not linger after the pointer leaves.
    bool mouse_tracked_ = false;
    // Press state for click-vs-drag disambiguation: a press on a row that
    // moves past the threshold becomes an OLE drag, a release without the move
    // is the click. Mirrors the dock tile gesture.
    bool press_active_ = false;
    int press_row_ = -1;
    POINT press_point_{};
    bool dragging_ = false;

    // Fonts created at open DPI, destroyed on close.
    HFONT font_ = nullptr;
    HFONT font_bold_ = nullptr;

    // ReadDirectoryChangesW + 500ms debounce (the T-040 watcher, reused).
    // Started for the current level while visible, stopped on close: a closed
    // panel must not watch anything.
    shell::LaunchpadWatcher watcher_;
};

}  // namespace dockforge::dock
