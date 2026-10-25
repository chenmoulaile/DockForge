// DockForge - weather hover-preview popup (T-075).
//
// The DoD's "悬停预览显示未来 3 天预报" rides the same 400ms dwell the
// thumbnail preview uses (dock::HoverTimer), but a forecast is text plus
// small glyphs, not DWM thumbnails - so this is a GDI popup in the
// folder-panel family (own window class, own WndProc, BitBlt double
// buffering) rather than a PreviewWindow. It knows nothing about tiles or
// hover state: the caller decides when to Show/Hide, exactly like the
// thumbnail popup's contract.
#pragma once

#include "dock/weather.h"

#include "platform/win_common.h"

#include <string>

namespace dockforge::dock {

class WeatherPopup {
  public:
    WeatherPopup() = default;
    ~WeatherPopup();

    WeatherPopup(const WeatherPopup&) = delete;
    WeatherPopup& operator=(const WeatherPopup&) = delete;

    // Registers the popup window class once per process (same pattern as
    // PreviewWindow/FolderPanel: a second window class whose WndProc must
    // never post WM_QUIT).
    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance);
    void Destroy();

    bool IsValid() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }
    bool Visible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }

    // Repaints with the given snapshot and places the popup so that its
    // bottom edge sits `gap_px` above `bottom_px`, horizontally centred on
    // `anchor_x_px` (clamped to the work area). `today` is the local date as
    // "YYYY-MM-DD" and drives the 今天/明天 labels.
    void Show(const weather::Snapshot& snapshot, const std::string& today, int anchor_x_px,
              int bottom_px, int dpi);

    void Hide();

    // Panel size for the current DPI, in physical pixels (the caller may need
    // it before the first Show).
    SIZE SizePx() const;

  private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);
    void Paint(HDC dc);

    HWND hwnd_ = nullptr;
    int width_px_ = 0;
    int height_px_ = 0;
    float scale_ = 1.0f;
    weather::Snapshot snapshot_{};
    std::string today_;
};

}  // namespace dockforge::dock
