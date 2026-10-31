// T-302 tray icon spike, round 2.
//
// Round 1 established that the classic tray probing recipe is dead on this host:
// enumerating Shell_TrayWnd -> TrayNotifyWnd -> SysPager -> ToolbarWindow32 and
// asking TB_BUTTONCOUNT returns -1, because the Windows 11 notification area is a
// XAML island (TrayNotifyWnd holds a DesktopWindowContentBridge, no SysPager and
// no toolbar). Button counting cannot be the assertion.
//
// The honest replacement is Shell_NotifyIconGetRect: it asks the shell where our
// own icon is on screen and fails when there is no icon. That is a direct answer
// to "did the icon land", not a proxy.
//
// Round 1 also showed the outside-click probe never fired: the click was posted to
// our window, but TrackPopupMenu runs its own modal pump, so the message sat in the
// queue while the menu blocked. A WM_TIMER is delivered inside that modal loop, so
// the dismissal probe has to be a timer.
//
// Questions:
//   Q1  Does a runtime-built HICON survive the shell round trip (GetRect succeeds)?
//   Q2  Does Shell_NotifyIconGetRect report a sensible rect, and does it fail after
//       NIM_DELETE? (existence proof that does not depend on window internals)
//   Q3  Does TrackPopupMenu from the tray callback dismiss on an outside click, and
//       does it return 0 for a dismissal versus an id for a selection?
//   Q4  What does NIM_ADD return when the icon already exists, and what does a
//       TaskbarCreated re-add do? (decides whether re-add must be unconditional)
//   Q5  Is NIF_SHOWTIP / NOTIFYICON_VERSION_4 accepted on this shell?
//
// Build:
//   g++ -std=c++17 -O2 -o tray_spike2.exe tray_spike2.cpp \
//       -luser32 -lshell32 -lgdi32 -lcomctl32

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <commctrl.h>
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
constexpr UINT_PTR kClickTimerId = 7;

HWND g_window = nullptr;
UINT g_taskbar_created = 0;
int g_callback_count = 0;
UINT g_last_callback_lparam = 0;
WPARAM g_last_callback_wparam = 0;
bool g_click_sent = false;
bool g_popup_seen = false;
RECT g_popup_rect{};
UINT g_menu_result = 0;
int g_menu_elapsed_ms = 0;

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
        const float sr = r * a;
        const float sg = g * a;
        const float sb = b * a;
        const float dr = p[2] / 255.0f;
        const float dg = p[1] / 255.0f;
        const float db = p[0] / 255.0f;
        p[0] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, sb + db * (1.0f - a)) * 255.0f));
        p[1] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, sg + dg * (1.0f - a)) * 255.0f));
        p[2] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, sr + dr * (1.0f - a)) * 255.0f));
        p[3] = static_cast<std::uint8_t>(std::lround(std::fmin(1.0f, out_a) * 255.0f));
    }

    void FillRoundRect(float left, float top, float right, float bottom, float radius,
                       float r, float g, float b, float a) {
        for (int y = static_cast<int>(std::floor(top)); y < static_cast<int>(std::ceil(bottom)); ++y) {
            for (int x = static_cast<int>(std::floor(left)); x < static_cast<int>(std::ceil(right)); ++x) {
                const float cx = x + 0.5f;
                const float cy = y + 0.5f;
                const float ix = std::fmin(std::fmax(cx, left + radius), right - radius);
                const float iy = std::fmin(std::fmax(cy, top + radius), bottom - radius);
                const float dx = cx - ix;
                const float dy = cy - iy;
                const float dist = std::sqrt(dx * dx + dy * dy);
                float coverage = std::fmin(1.0f, std::fmax(0.0f, radius - dist + 0.5f));
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
                float coverage = std::fmin(1.0f, std::fmax(0.0f, radius - dist + 0.5f));
                if (coverage > 0.0f) Blend(x, y, r, g, b, a * coverage);
            }
        }
    }
};

HICON MakeIcon(const Canvas& canvas) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = canvas.width;
    header.bV5Height = -canvas.height;
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

    // A 32bpp colour bitmap carries its own alpha, but the mask still has to be a
    // real bitmap: leaving it null makes CreateIconIndirect fail on some shells.
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

NOTIFYICONDATAW MakeData(HICON icon) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon;
    wcscpy_s(data.szTip, L"DockForge");
    return data;
}

// The existence proof. Returns false when the shell has no icon for this
// (hWnd, uID) pair, which is exactly what NIM_DELETE should cause.
bool QueryIconRect(HWND hwnd, UINT id, RECT* out, HRESULT* hr_out) {
    NOTIFYICONIDENTIFIER identifier{};
    identifier.cbSize = sizeof(identifier);
    identifier.hWnd = hwnd;
    identifier.uID = id;
    RECT rect{};
    const HRESULT hr = Shell_NotifyIconGetRect(&identifier, &rect);
    if (hr_out != nullptr) *hr_out = hr;
    if (SUCCEEDED(hr) && out != nullptr) *out = rect;
    return SUCCEEDED(hr);
}

UINT ShowTrayMenu(HWND hwnd, int x, int y, int* elapsed_out) {
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
    // The documented pairing: a null post after the menu closes, otherwise the
    // next click on the tray icon is swallowed by the leftover menu loop.
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (elapsed_out != nullptr) *elapsed_out = static_cast<int>(GetTickCount64() - start);
    DestroyMenu(menu);
    return id;
}

// Runs inside TrackPopupMenu's modal pump, which is the only place a dismissal
// probe can act: a message posted to our window would wait until the menu closed.
void ProbeClickFromTimer() {
    HWND popup = FindWindowW(L"#32768", nullptr);
    if (popup != nullptr && !g_popup_seen) {
        g_popup_seen = true;
        GetWindowRect(popup, &g_popup_rect);
        Log("[Q3] popup rect=%ld,%ld,%ld,%ld", g_popup_rect.left, g_popup_rect.top,
            g_popup_rect.right, g_popup_rect.bottom);
    }
    if (!g_popup_seen || g_click_sent) return;
    // Aim far outside the popup so the dismissal path is what gets exercised.
    const int x = g_popup_rect.left > 600 ? g_popup_rect.left - 400 : g_popup_rect.right + 400;
    const int y = g_popup_rect.bottom + 300;
    g_click_sent = true;
    Log("[Q3] clicking outside at %d,%d", x, y);
    SetCursorPos(x, y);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kCallbackMessage) {
        ++g_callback_count;
        g_last_callback_lparam = static_cast<UINT>(lp);
        g_last_callback_wparam = wp;
        return 0;
    }
    if (g_taskbar_created != 0 && message == g_taskbar_created) {
        Log("[Q4] TaskbarCreated received");
        RECT rect{};
        HRESULT hr = E_FAIL;
        const bool existed = QueryIconRect(hwnd, kTrayIconId, &rect, &hr);
        Log("[Q4] before re-add: rect ok=%d hr=0x%08lx", existed ? 1 : 0, static_cast<unsigned long>(hr));
        HICON icon = static_cast<HICON>(GetPropW(hwnd, L"tray-icon"));
        NOTIFYICONDATAW data = MakeData(icon);
        const BOOL added = Shell_NotifyIconW(NIM_ADD, &data);
        const BOOL modified = Shell_NotifyIconW(NIM_MODIFY, &data);
        const bool after = QueryIconRect(hwnd, kTrayIconId, &rect, &hr);
        Log("[Q4] re-add=%d modify=%d after: rect ok=%d hr=0x%08lx", added ? 1 : 0, modified ? 1 : 0,
            after ? 1 : 0, static_cast<unsigned long>(hr));
        return 0;
    }
    if (message == WM_TIMER && wp == kClickTimerId) {
        ProbeClickFromTimer();
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

}  // namespace

int main() {
    g_log = std::fopen("tray_spike2.log", "w");
    Log("tray spike round 2");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const int icon_px = GetSystemMetrics(SM_CXSMICON);
    Log("[Q5] SM_CXSMICON=%d SM_CYSMICON=%d", icon_px, GetSystemMetrics(SM_CYSMICON));

    Canvas canvas(icon_px, icon_px);
    const float plate = static_cast<float>(icon_px);
    canvas.FillRoundRect(plate * 0.04f, plate * 0.20f, plate * 0.96f, plate * 0.80f, plate * 0.20f,
                         0.16f, 0.18f, 0.22f, 1.0f);
    for (int i = 0; i < 3; ++i) {
        canvas.FillCircle(plate * (0.28f + 0.22f * static_cast<float>(i)), plate * 0.50f, plate * 0.075f,
                          0.96f, 0.97f, 0.98f, 1.0f);
    }
    HICON icon = MakeIcon(canvas);
    Log("[Q1] icon=%p", static_cast<void*>(icon));

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"TraySpike2Window";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("[setup] RegisterClassExW failed err=%lu", GetLastError());
        return 1;
    }
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW, L"TraySpike2Window", L"tray spike 2", WS_POPUP, 0, 0, 1, 1,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (g_window == nullptr) {
        Log("[setup] CreateWindowExW failed err=%lu", GetLastError());
        return 1;
    }
    SetPropW(g_window, L"tray-icon", icon);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    // ---- Q2: does the icon exist, per the shell's own answer? --------------
    RECT rect{};
    HRESULT hr = E_FAIL;
    const bool before_add = QueryIconRect(g_window, kTrayIconId, &rect, &hr);
    Log("[Q2] before NIM_ADD: exists=%d hr=0x%08lx (expect 0)", before_add ? 1 : 0,
        static_cast<unsigned long>(hr));

    NOTIFYICONDATAW data = MakeData(icon);
    const BOOL added = Shell_NotifyIconW(NIM_ADD, &data);
    data.uVersion = NOTIFYICON_VERSION_4;
    const BOOL versioned = Shell_NotifyIconW(NIM_SETVERSION, &data);
    Sleep(400);
    const bool after_add = QueryIconRect(g_window, kTrayIconId, &rect, &hr);
    Log("[Q2] NIM_ADD=%d NIM_SETVERSION=%d exists=%d hr=0x%08lx rect=%ld,%ld,%ld,%ld size=%ldx%ld",
        added ? 1 : 0, versioned ? 1 : 0, after_add ? 1 : 0, static_cast<unsigned long>(hr), rect.left,
        rect.top, rect.right, rect.bottom, rect.right - rect.left, rect.bottom - rect.top);

    // ---- Q2b: the version-4 payload layout --------------------------------
    PostMessageW(g_window, kCallbackMessage, MAKEWPARAM(1200, 800),
                 MAKELPARAM(WM_RBUTTONUP, kTrayIconId));
    MSG msg{};
    const DWORD deadline = GetTickCount() + 300;
    while (GetTickCount() < deadline && g_callback_count == 0) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
    Log("[Q2b] callbacks=%d anchor=%d,%d event=0x%x id=%d", g_callback_count,
        static_cast<short>(LOWORD(g_last_callback_wparam)), static_cast<short>(HIWORD(g_last_callback_wparam)),
        LOWORD(g_last_callback_lparam), HIWORD(g_last_callback_lparam));

    // ---- Q3: menu + outside-click dismissal --------------------------------
    SetTimer(g_window, kClickTimerId, 150, nullptr);
    g_menu_result = ShowTrayMenu(g_window, 1200, 800, &g_menu_elapsed_ms);
    KillTimer(g_window, kClickTimerId);
    Log("[Q3] menu result=%u elapsed_ms=%d popup_seen=%d click_sent=%d", g_menu_result, g_menu_elapsed_ms,
        g_popup_seen ? 1 : 0, g_click_sent ? 1 : 0);

    // ---- Q4: re-add semantics ---------------------------------------------
    SendMessageW(g_window, g_taskbar_created, 0, 0);

    // ---- Q6: delete and confirm the shell forgets it -----------------------
    const BOOL removed = Shell_NotifyIconW(NIM_DELETE, &data);
    Sleep(400);
    const bool after_delete = QueryIconRect(g_window, kTrayIconId, &rect, &hr);
    Log("[Q6] NIM_DELETE=%d exists=%d hr=0x%08lx (expect 0)", removed ? 1 : 0, after_delete ? 1 : 0,
        static_cast<unsigned long>(hr));

    const bool q1 = icon != nullptr;
    const bool q2 = !before_add && added && after_add;
    const bool q3 = g_popup_seen && g_click_sent && g_menu_result == 0;
    const bool q6 = removed && !after_delete;
    Log("verdict: Q1_icon=%s Q2_exists=%s Q3_menu_dismiss=%s Q6_delete=%s",
        q1 ? "PASS" : "FAIL", q2 ? "PASS" : "FAIL", q3 ? "PASS" : "FAIL", q6 ? "PASS" : "FAIL");

    DestroyIcon(icon);
    DestroyWindow(g_window);
    if (g_log != nullptr) std::fclose(g_log);
    return 0;
}
