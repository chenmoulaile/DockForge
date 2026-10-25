// DockForge - notification history panel (T-076).
//
// The DoD puts this behind "click the Finder Bar clock", but the Finder Bar is
// M2/TODO, so it hangs off the work area's top-right corner instead - exactly the
// precedent T-068 set for the hardware panel (TASKS.md:628). When M2 lands, the
// anchor moves and nothing else in this file changes.
//
// Same split as the banner: PanelLayout is pure (rows, hit-testing, height) and
// NotificationPanel is the GDI window. The layout is pure because the one thing
// that can silently break here is the mapping from a click to a row, and that is
// not observable from a screenshot.
#pragma once

#include "core/config.h"
#include "notifications/notification_types.h"
#include "platform/win_common.h"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace dockforge::notifications {

// DIP layout.
inline constexpr float kPanelWidthDip = 360.0f;
inline constexpr float kPanelHeaderHeightDip = 40.0f;
inline constexpr float kPanelRowHeightDip = 52.0f;
inline constexpr float kPanelFooterHeightDip = 36.0f;
inline constexpr float kPanelEdgeMarginDip = 12.0f;
inline constexpr float kPanelMaxRowsDip = 8.0f;  // beyond this the list scrolls
inline constexpr float kPanelClearWidthDip = 64.0f;

// What a click on the panel hit.
struct PanelHit {
    enum class Kind { None, Row, Clear, Close } kind = Kind::None;
    std::size_t index = 0;  // valid for Kind::Row
};

// Pure layout for the history panel. `count` is how many rows the store holds.
struct PanelLayout {
    std::size_t visible_rows = 0;   // rows that fit without scrolling
    std::size_t first_row = 0;      // index of the topmost drawn row
    double height_dip = 0.0;

    // Computes the layout for `count` rows with `scroll` rows already skipped.
    static PanelLayout Compute(std::size_t count, std::size_t scroll);

    // Maps a click at `y_dip` (from the plate's top edge) to a row, the clear
    // button or nothing.
    PanelHit HitTest(double y_dip, double x_dip, double width_dip) const;
};

class NotificationPanel {
  public:
    NotificationPanel() = default;
    ~NotificationPanel();

    NotificationPanel(const NotificationPanel&) = delete;
    NotificationPanel& operator=(const NotificationPanel&) = delete;

    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance);
    void Destroy();

    bool IsValid() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }

    // C-21, same rule as the banner: the panel never reads the power state.
    void SetMaterialTier(core::MaterialLevel tier);

    // Points the panel at the live history. The store outlives the panel; the
    // panel only reads it during Paint/Refresh.
    void SetItems(const std::vector<NotificationItem>* items);
    void SetAccessStatus(AccessStatus status);

    // Clicking a row asks the app to focus that notification's sender.
    void SetActivateHandler(std::function<void(const NotificationItem&)> handler) {
        on_activate_ = std::move(handler);
    }
    // Clicking 清除 empties the history (in memory only).
    void SetClearHandler(std::function<void()> handler) { on_clear_ = std::move(handler); }

    void Open(int dpi);
    void Close();
    void Refresh(int dpi);

    // Scrolls the list; returns true when the visible window moved.
    bool ScrollBy(int rows);

    // Probe payload: counts only, never notification text.
    std::string SnapshotJson() const;

  private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC dc);
    void Relayout();

    HWND hwnd_ = nullptr;
    const std::vector<NotificationItem>* items_ = nullptr;
    AccessStatus access_ = AccessStatus::Unspecified;
    core::MaterialLevel tier_ = core::MaterialLevel::Solid;
    std::function<void(const NotificationItem&)> on_activate_;
    std::function<void()> on_clear_;
    PanelLayout layout_{};
    std::size_t scroll_ = 0;
    int width_px_ = 0;
    int height_px_ = 0;
    float scale_ = 1.0f;
};

}  // namespace dockforge::notifications
