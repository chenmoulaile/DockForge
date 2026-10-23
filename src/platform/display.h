// DockForge - display enumeration and DPI-aware dock placement.
//
// Everything here is in PHYSICAL PIXELS in virtual-screen coordinates. The
// process is per-monitor-v2 DPI aware, so these are the same numbers the window
// manager uses and no virtualization sits in between. Logical (DIP) sizes only
// appear at the API boundary of ComputeDockPlacement, which converts once, at
// the edge, so no other code has to remember which unit it holds.
#pragma once

#include "platform/win_common.h"

#include <vector>

namespace dockforge::platform {

// Which edge of the work area the dock hugs. Mirrors core::DockPosition but
// lives here so platform does not depend on the config module.
enum class DockEdge { Bottom, Left, Right };

struct DisplayInfo {
    HMONITOR handle = nullptr;
    std::wstring device;  // \\.\DISPLAY1
    RECT monitor{};       // full extent, physical px
    RECT work{};          // extent minus taskbar/appbars, physical px
    // MDT_EFFECTIVE_DPI: what the user's scaling setting produces. This is the
    // one layout must use.
    UINT effective_dpi = 96;
    // MDT_RAW_DPI: the panel's physical density. Not used for layout; kept
    // because a 243 raw / 192 effective display is how "200% scaling on a
    // 2880-wide panel" looks, and that difference explains blur reports.
    UINT raw_dpi = 96;
    bool primary = false;

    int Width() const { return monitor.right - monitor.left; }
    int Height() const { return monitor.bottom - monitor.top; }
    int WorkWidth() const { return work.right - work.left; }
    int WorkHeight() const { return work.bottom - work.top; }
    float Scale() const { return static_cast<float>(effective_dpi ? effective_dpi : 96) / 96.0f; }
};

// All monitors, primary first, then by position (top, then left) so the order is
// stable across calls and a display index in a log or config means the same
// thing twice in a row.
std::vector<DisplayInfo> EnumerateDisplays();

// The monitor flagged MONITORINFOF_PRIMARY. False if enumeration failed.
bool PrimaryDisplay(DisplayInfo& out);

// The monitor currently hosting `hwnd` (MONITOR_DEFAULTTONEAREST, so a window
// dragged off-screen still resolves).
bool DisplayForWindow(HWND hwnd, DisplayInfo& out);

// The monitor containing a physical-pixel point.
bool DisplayForPoint(POINT point, DisplayInfo& out);

struct DockPlacement {
    RECT rect{};             // physical px, virtual-screen coordinates
    int width_px = 0;
    int height_px = 0;
    UINT dpi = 96;
    float scale = 1.0f;
    // True when the work area was too small for the requested logical size and
    // the dock had to be shrunk. Callers log this; silently clipping a dock is
    // how "it looks fine on my machine" bugs ship.
    bool clamped = false;
};

// Places a `logical_width` x `logical_height` (DIP) dock against `edge` of the
// display's work area, inset by `margin_dip` and centered along that edge.
// The result is always fully inside the work area.
DockPlacement ComputeDockPlacement(const DisplayInfo& display, DockEdge edge, float logical_width,
                                   float logical_height, float margin_dip);

// Logical pixels for a physical length at a given DPI.
float PixelsToDip(int pixels, UINT dpi);
// Physical pixels for a DIP length, rounded to nearest.
int DipToPixels(float dip, UINT dpi);

}  // namespace dockforge::platform
