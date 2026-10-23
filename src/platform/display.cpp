#include "platform/display.h"

#include "core/log.h"

#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>

namespace dockforge::platform {
namespace {

BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<DisplayInfo>*>(param);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return TRUE;

    DisplayInfo display;
    display.handle = handle;
    display.device = info.szDevice;
    display.monitor = info.rcMonitor;
    display.work = info.rcWork;
    display.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT effective = 96;
    UINT raw = 96;
    if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &effective, &raw))) {
        // GetDpiForMonitor fails on a detached or virtual display; the system DPI
        // is the honest fallback rather than a guess.
        effective = GetDpiForSystem();
        raw = effective;
    }
    if (effective == 0) effective = 96;
    UINT raw_x = effective;
    UINT raw_y = effective;
    GetDpiForMonitor(handle, MDT_RAW_DPI, &raw_x, &raw_y);
    display.effective_dpi = effective;
    display.raw_dpi = raw_x ? raw_x : effective;

    out->push_back(std::move(display));
    return TRUE;
}

}  // namespace

std::vector<DisplayInfo> EnumerateDisplays() {
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor,
                        reinterpret_cast<LPARAM>(&displays));

    // Primary first, then top-to-bottom / left-to-right. A stable order matters:
    // logs and future per-display settings address displays by index.
    std::sort(displays.begin(), displays.end(), [](const DisplayInfo& a, const DisplayInfo& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.monitor.top != b.monitor.top) return a.monitor.top < b.monitor.top;
        if (a.monitor.left != b.monitor.left) return a.monitor.left < b.monitor.left;
        return a.device < b.device;
    });
    return displays;
}

bool PrimaryDisplay(DisplayInfo& out) {
    const auto displays = EnumerateDisplays();
    for (const auto& display : displays) {
        if (display.primary) {
            out = display;
            return true;
        }
    }
    if (!displays.empty()) {
        out = displays.front();
        return true;
    }
    return false;
}

bool DisplayForWindow(HWND hwnd, DisplayInfo& out) {
    if (!hwnd) return false;
    const HMONITOR handle = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!handle) return false;

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return false;

    out = DisplayInfo{};
    out.handle = handle;
    out.device = info.szDevice;
    out.monitor = info.rcMonitor;
    out.work = info.rcWork;
    out.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT effective = 96;
    UINT raw = 96;
    if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &effective, &raw))) {
        effective = GetDpiForWindow(hwnd);
        raw = effective;
    }
    if (effective == 0) effective = 96;
    UINT raw_x = effective;
    UINT raw_y = effective;
    GetDpiForMonitor(handle, MDT_RAW_DPI, &raw_x, &raw_y);
    out.effective_dpi = effective;
    out.raw_dpi = raw_x ? raw_x : effective;
    return true;
}

bool DisplayForPoint(POINT point, DisplayInfo& out) {
    const HMONITOR handle = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (!handle) return false;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(handle, &info)) return false;

    out = DisplayInfo{};
    out.handle = handle;
    out.device = info.szDevice;
    out.monitor = info.rcMonitor;
    out.work = info.rcWork;
    out.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    UINT effective = 96;
    UINT raw = 96;
    if (FAILED(GetDpiForMonitor(handle, MDT_EFFECTIVE_DPI, &effective, &raw))) {
        effective = GetDpiForSystem();
    }
    if (effective == 0) effective = 96;
    UINT raw_x = effective;
    UINT raw_y = effective;
    GetDpiForMonitor(handle, MDT_RAW_DPI, &raw_x, &raw_y);
    out.effective_dpi = effective;
    out.raw_dpi = raw_x ? raw_x : effective;
    return true;
}

float PixelsToDip(int pixels, UINT dpi) {
    const float safe = dpi ? static_cast<float>(dpi) : 96.0f;
    return static_cast<float>(pixels) * 96.0f / safe;
}

int DipToPixels(float dip, UINT dpi) {
    const float safe = dpi ? static_cast<float>(dpi) : 96.0f;
    return static_cast<int>(std::lround(dip * safe / 96.0f));
}

DockPlacement ComputeDockPlacement(const DisplayInfo& display, DockEdge edge, float logical_width,
                                   float logical_height, float margin_dip) {
    DockPlacement placement;
    placement.dpi = display.effective_dpi ? display.effective_dpi : 96;
    placement.scale = static_cast<float>(placement.dpi) / 96.0f;

    // Work area, not monitor rect: the dock must not sit under the taskbar.
    RECT work = display.work;
    if (work.right <= work.left || work.bottom <= work.top) {
        work = display.monitor;
    }

    const int margin = DipToPixels(margin_dip, placement.dpi);
    int width = DipToPixels(logical_width, placement.dpi);
    int height = DipToPixels(logical_height, placement.dpi);

    // Clamp before positioning so the result is always inside the work area.
    // A dock wider than the screen is worse than a smaller dock.
    const int max_width = std::max(0L, (work.right - work.left) - 2L * margin);
    const int max_height = std::max(0L, (work.bottom - work.top) - 2L * margin);
    if (width > max_width) {
        width = max_width;
        placement.clamped = true;
    }
    if (height > max_height) {
        height = max_height;
        placement.clamped = true;
    }

    int x = work.left;
    int y = work.top;
    switch (edge) {
        case DockEdge::Bottom:
            x = work.left + ((work.right - work.left) - width) / 2;
            y = work.bottom - margin - height;
            break;
        case DockEdge::Left:
            x = work.left + margin;
            y = work.top + ((work.bottom - work.top) - height) / 2;
            break;
        case DockEdge::Right:
            x = work.right - margin - width;
            y = work.top + ((work.bottom - work.top) - height) / 2;
            break;
    }

    placement.rect = RECT{x, y, x + width, y + height};
    placement.width_px = width;
    placement.height_px = height;
    return placement;
}

}  // namespace dockforge::platform
