// T-302 tray icon spike.
//
// The app currently has no tray presence at all: grep for Shell_NotifyIcon over
// src/ returns nothing, so quitting requires Task Manager. Before writing the
// module, four questions have to be answered with measurements, because each one
// has a failure mode that only shows up on a real desktop:
//
//   Q1  Can a HICON built at runtime (CreateIconIndirect over a 32bpp DIB) be
//       handed to Shell_NotifyIconW, and does the shell keep the pixels? A black
//       or empty icon is the classic symptom of a mask that was never filled.
//   Q2  Does the icon actually appear in the notification area? Verified by
//       counting the toolbar buttons in the tray across NIM_ADD/NIM_DELETE
//       rather than by trusting the return value.
//   Q3  With NOTIFYICON_VERSION_4, where do the cursor coordinates live, and does
//       a TrackPopupMenu from the callback dismiss on an outside click? The
//       "menu never closes" bug is the reason the documented recipe exists.
//   Q4  After explorer restarts it broadcasts "TaskbarCreated" and every icon is
//       gone. Does re-adding inside that handler restore the count?
//
// Build:
//   g++ -std=c++17 -O2 -o tray_spike.exe tray_spike.cpp -luser32 -lshell32 -lgdi32

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <commctrl.h>  // TB_BUTTONCOUNT: the only honest way to count tray buttons
#include <shellapi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    if (g_log != nullptr) {
        std::fputs(line, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

constexpr UINT kCallbackMessage = WM_APP + 100;
constexpr UINT kTrayIconId = 1;

HWND g_window = nullptr;
UINT g_taskbar_created = 0;
int g_callback_count = 0;
UINT g_last_callback_lparam = 0;
WPARAM g_last_callback_wparam = 0;

// ---------------------------------------------------------------- icon drawing
//
// 32bpp premultiplied BGRA. Everything here is deliberate: a rounded plate with
// three bars is the same glyph the Finder Bar uses for its system menu, so the
// tray icon reads as the same product rather than a generic dot.
struct Canvas {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    Canvas(int w, int h) : width(w), height(h), pixels(static_cast<std::size_t>(w * h * 4), 0) {}

    void Blend(int x, int y, float r, float g, float b, float a) {
        if (x < 0 || y < 0 || x >= width || y >= height || a <= 0.0f) return;
        auto* p = &pixels[(static_cast<std::size_t>(y) * width + x) * 4];
        const float dst_a = p[3] / 255.0f;
        const float out_a = a + dst_a * (1.0f - a);
        if (out_a <= 0.0f) return;
        // Premultiplied source over premultiplied destination.
        const float sr = r * a;
        const float sg = g * a;
        const float sb = b * a;
        const float dr = p[2] / 255.0f;
        const float dg = p[1] / 255.0f;
        const float db = p[0] / 255.0f;
        const float orr = sr + dr * (1.0f - a);
        const float og = sg + dg * (1.0f - a);
        const float ob = sb + db * (1.0f - a);
        p[0] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, ob) * 255.0f));
        p[1] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, og) * 255.0f));
        p[2] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, orr) * 255.0f));
        p[3] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, out_a) * 255.0f));
    }

    void FillRoundRect(float left, float top, float right, float bottom, float radius,
                       float r, float g, float b, float a) {
        for (int y = static_cast<int>(std::floor(top)); y < static_cast<int>(std::ceil(bottom)); ++y) {
            for (int x = static_cast<int>(std::floor(left)); x < static_cast<int>(std::ceil(right)); ++x) {
                const float cx = x + 0.5f;
                const float cy = y + 0.5f;
                // Distance to the rounded rectangle: clamp the point into the
                // inner box, then measure from there.
                const float ix = std::fmin(std::fmax(cx, left + radius), right - radius);
                const float iy = std::fmin(std::fmax(cy, top + radius), bottom - radius);
                const float dx = cx - ix;
                const float dy = cy - iy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                // One pixel of coverage ramp so the corners are not stair-stepped.
                float coverage = radius - dist + 0.5f;
                coverage = std::fmin(1.0f, std::fmax(0.0f, coverage));
                if (coverage > 0.0f) Blend(x, y, r, g, b, a * coverage);
            }
        }
    }

    void FillCircle(float cx, float cy, float radius, float r, float g, float b, float a) {
        for (int y = static_cast<int>(std::floor(cy - radius)); y <= static_cast<int>(std::ceil(cy + radius)); ++y) {
            for (int x = static_cast<int>(std::floor(cx - radius)); x <= static_cast<int>(std::ceil(cx + radius)); ++x) {
                const float dx = x + 0.5f - cx;
                const float dy = y + 0.5f - cy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                float coverage = radius - dist + 0.5f;
                coverage = std::fmin(1.0f, std::fmax(0.0f, coverage));
                if (coverage > 0.0f) Blend(x, y, r, g, b, a * coverage);
            }
        }
    }
};

// Builds a HICON from the canvas. The mask is filled with zeros (opaque
// everywhere) because a 32bpp colour bitmap carries its own alpha; leaving the
// mask undefined is what produces a black square on some shells.
HICON MakeIcon(const Canvas& canvas) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = canvas.width;
    header.bV5Height = -canvas.height;  // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP colour = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
                                      &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (colour == nullptr || bits == nullptr) return nullptr;
    std::memcpy(bits, canvas.pixels.data(), canvas.pixels.size());

    HBITMAP mask = CreateBitmap(canvas.width, canvas.height, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = colour;
    info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(colour);
    DeleteObject(mask);
    return icon;
}

// ---------------------------------------------------------------- tray probing

// Counts the buttons in the notification-area toolbar. This is the only way to
// prove an icon actually landed: Shell_NotifyIconW returns TRUE even when the
// shell silently ignores the request (seen when the icon handle is invalid).
int TrayButtonCount() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (tray == nullptr) return -1;
    HWND notify = FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify == nullptr) return -1;
    HWND pager = FindWindowExW(notify, nullptr, L"SysPager", nullptr);
    if (pager == nullptr) return -1;
    HWND toolbar = FindWindowExW(pager, nullptr, L"ToolbarWindow32", nullptr);
    if (toolbar == nullptr) return -1;
    return static_cast<int>(SendMessageW(toolbar, TB_BUTTONCOUNT, 0, 0));
}

// Reads the pixels back out of an HICON. A runtime-built icon that the shell
// accepted can still be blank if the alpha channel or the mask is wrong, so the
// assertion is on the content, not on the handle being non-null.
bool IconHasContent(HICON icon, int* opaque_out, int* coloured_out) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return false;
    BITMAP bitmap{};
    if (GetObjectW(info.hbmColor, sizeof(bitmap), &bitmap) == 0) {
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        return false;
    }
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bitmap.bmWidth;
    bmi.bmiHeader.biHeight = -bitmap.bmHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(bitmap.bmWidth) * bitmap.bmHeight * 4, 0);
    HDC screen = GetDC(nullptr);
    const int rows = GetDIBits(screen, info.hbmColor, 0, static_cast<UINT>(bitmap.bmHeight),
                               pixels.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    if (rows != bitmap.bmHeight) return false;

    int opaque = 0;
    int coloured = 0;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i + 3] > 200) {
            ++opaque;
            // Anything not near-black counts: the plate is a dark slate, the bars
            // are near-white, so a black square fails this and a real glyph passes.
            if (pixels[i] > 40 || pixels[i + 1] > 40 || pixels[i + 2] > 40) ++coloured;
        }
    }
    if (opaque_out != nullptr) *opaque_out = opaque;
    if (coloured_out != nullptr) *coloured_out = coloured;
    return true;
}

// ---------------------------------------------------------------- window proc

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kCallbackMessage) {
        ++g_callback_count;
        g_last_callback_lparam = static_cast<UINT>(lp);
        g_last_callback_wparam = wp;
        return 0;
    }
    if (g_taskbar_created != 0 && message == g_taskbar_created) {
        Log("[Q4] TaskbarCreated received, re-adding");
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd;
        data.uID = kTrayIconId;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = kCallbackMessage;
        data.hIcon = static_cast<HICON>(GetPropW(hwnd, L"tray-icon"));
        wcscpy_s(data.szTip, L"DockForge (re-added)");
        Log("[Q4] re-add result=%d", Shell_NotifyIconW(NIM_ADD, &data) ? 1 : 0);
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

// Pops the menu the way the shell documentation says to: SetForegroundWindow
// first, TrackPopupMenu, then a null post. Skipping either step leaves the menu
// on screen until the user clicks twice.
UINT ShowTrayMenu(HWND hwnd, int x, int y, int* out_elapsed_ms) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1001, L"Show Dock");
    AppendMenuW(menu, MF_STRING, 1002, L"Hide Dock");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 1003, L"Settings\u2026");
    AppendMenuW(menu, MF_STRING, 1004, L"Quit DockForge");
    SetMenuDefaultItem(menu, 1003, FALSE);

    const ULONGLONG start = GetTickCount64();
    SetForegroundWindow(hwnd);
    const UINT id = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd, nullptr));
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (out_elapsed_ms != nullptr) *out_elapsed_ms = static_cast<int>(GetTickCount64() - start);
    DestroyMenu(menu);
    return id;
}

// Finds the tracked popup (#32768) and clicks well outside it, so the dismissal
// path is exercised rather than assumed.
bool ClickOutsidePopup(int x, int y) {
    HWND popup = nullptr;
    for (int attempt = 0; attempt < 20 && popup == nullptr; ++attempt) {
        popup = FindWindowW(L"#32768", nullptr);
        if (popup == nullptr) Sleep(20);
    }
    if (popup == nullptr) return false;
    RECT rect{};
    GetWindowRect(popup, &rect);
    Log("[Q3] popup rect=%ld,%ld,%ld,%ld", rect.left, rect.top, rect.right, rect.bottom);
    SetCursorPos(x, y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    return true;
}

}  // namespace

int main() {
    g_log = std::fopen("tray_spike.log", "w");
    Log("tray spike start");

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const int small_cx = GetSystemMetrics(SM_CXSMICON);
    const int small_cy = GetSystemMetrics(SM_CYSMICON);
    Log("[Q5] SM_CXSMICON=%d SM_CYSMICON=%d (system dpi=%d)", small_cx, small_cy,
        GetDeviceCaps(GetDC(nullptr), LOGPIXELSX));

    // ---- Q1: build the icon and prove it has content -----------------------
    const int icon_px = small_cx > 0 ? small_cx : 16;
    Canvas canvas(icon_px, icon_px);
    const float plate = static_cast<float>(icon_px);
    canvas.FillRoundRect(plate * 0.04f, plate * 0.20f, plate * 0.96f, plate * 0.80f,
                         plate * 0.20f, 0.16f, 0.18f, 0.22f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        const float cx = plate * (0.28f + 0.22f * static_cast<float>(i));
        canvas.FillCircle(cx, plate * 0.50f, plate * 0.075f, 0.96f, 0.97f, 0.98f, 1.0f);
    }
    HICON icon = MakeIcon(canvas);
    int opaque = 0;
    int coloured = 0;
    const bool content_ok = icon != nullptr && IconHasContent(icon, &opaque, &coloured);
    Log("[Q1] icon=%p opaque=%d coloured=%d", static_cast<void*>(icon), opaque, coloured);

    // ---- window --------------------------------------------------------------
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TraySpikeWindow";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("[setup] RegisterClassExW failed err=%lu", GetLastError());
        return 1;
    }
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"TraySpikeWindow", L"tray spike", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (g_window == nullptr) {
        Log("[setup] CreateWindowExW failed err=%lu", GetLastError());
        return 1;
    }
    SetPropW(g_window, L"tray-icon", icon);

    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    // ---- Q2: does the icon land in the tray? --------------------------------
    const int before = TrayButtonCount();
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon;
    wcscpy_s(data.szTip, L"DockForge");
    const BOOL added = Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    const BOOL versioned = Shell_NotifyIconW(NIM_SETVERSION, &data);
    Sleep(300);
    const int after = TrayButtonCount();
    Log("[Q2] NIM_ADD=%d NIM_SETVERSION=%d buttons before=%d after=%d delta=%d", added ? 1 : 0,
        versioned ? 1 : 0, before, after, after - before);

    // ---- Q2b: does the callback fire, and what does it carry? --------------
    // A synthetic post proves the message reaches the window and shows the
    // version-4 payload layout; a real right click cannot be injected here.
    PostMessageW(g_window, kCallbackMessage, MAKEWPARAM(1200, 800), MAKELPARAM(WM_RBUTTONUP, kTrayIconId));
    MSG msg{};
    const DWORD deadline = GetTickCount() + 300;
    while (GetTickCount() < deadline && g_callback_count == 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    Log("[Q2b] callbacks=%d wparam=0x%llx (x=%d y=%d) lparam=0x%llx (msg=0x%x id=%d)",
        g_callback_count, static_cast<unsigned long long>(g_last_callback_wparam),
        static_cast<short>(LOWORD(g_last_callback_wparam)),
        static_cast<short>(HIWORD(g_last_callback_wparam)),
        static_cast<unsigned long long>(g_last_callback_lparam), LOWORD(g_last_callback_lparam),
        HIWORD(g_last_callback_lparam));

    // ---- Q3: menu from the tray callback, and outside-click dismissal ------
    // The menu is shown from a worker-free path: a timer posts the request so the
    // main thread is inside its own message pump when TrackPopupMenu blocks.
    struct MenuProbe {
        UINT id = 0;
        int elapsed_ms = 0;
        bool outside_clicked = false;
    } probe;
    PostMessageW(g_window, WM_APP + 101, 0, 0);
    // Run one modal-ish pass: handle WM_APP+101 by showing the menu, then click
    // outside from this same thread once the popup exists.
    bool handled_menu = false;
    const DWORD menu_deadline = GetTickCount() + 4000;
    while (GetTickCount() < menu_deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_APP + 101) {
                handled_menu = true;
                // Click outside first, from a posted message, so it lands while
                // TrackPopupMenu is inside its modal loop.
                PostMessageW(g_window, WM_APP + 102, 0, 0);
                probe.id = ShowTrayMenu(g_window, 1200, 800, &probe.elapsed_ms);
                continue;
            }
            if (msg.message == WM_APP + 102) {
                // Give the popup a moment to appear before aiming at it.
                Sleep(120);
                probe.outside_clicked = ClickOutsidePopup(1400, 1100);
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (handled_menu && probe.id != 0) break;
        if (handled_menu && !probe.outside_clicked && probe.elapsed_ms > 0) break;
        Sleep(10);
    }
    Log("[Q3] menu handled=%d selected=%u elapsed_ms=%d outside_clicked=%d", handled_menu ? 1 : 0,
        probe.id, probe.elapsed_ms, probe.outside_clicked ? 1 : 0);

    // ---- Q4: explorer restart re-registration ------------------------------
    const int before_broadcast = TrayButtonCount();
    SendMessageW(g_window, g_taskbar_created, 0, 0);
    Sleep(300);
    const int after_broadcast = TrayButtonCount();
    Log("[Q4] buttons before=%d after=%d", before_broadcast, after_broadcast);

    // ---- cleanup -----------------------------------------------------------
    const BOOL removed = Shell_NotifyIconW(NIM_DELETE, &data);
    Sleep(300);
    const int after_delete = TrayButtonCount();
    Log("[Q6] NIM_DELETE=%d buttons after=%d", removed ? 1 : 0, after_delete);

    Log("verdict: Q1_icon=%s Q2_added=%s Q3_menu=%s Q4_readd=%s Q6_delete=%s",
        (icon != nullptr && content_ok && opaque > 0 && coloured > 0) ? "PASS" : "FAIL",
        (added && after > before) ? "PASS" : "FAIL",
        (probe.outside_clicked && probe.id == 0) ? "PASS" : "FAIL",
        (after_broadcast >= before_broadcast) ? "PASS" : "FAIL",
        (removed && after_delete < after_broadcast) ? "PASS" : "FAIL");

    DestroyIcon(icon);
    DestroyWindow(g_window);
    if (g_log != nullptr) std::fclose(g_log);
    return 0;
}
