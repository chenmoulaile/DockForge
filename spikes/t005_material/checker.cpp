// spikes/t005_material/checker.cpp - a checkerboard backdrop for measuring frost.
//
// A flat-colour backdrop can only prove "the plate is translucent". To prove the
// plate actually BLURS, the backdrop needs high-frequency detail that blur
// destroys and an alpha blend preserves. 8px cells at every DPI scale are fine
// enough that an 18 DIP Gaussian erases them and coarse enough that a screenshot
// still resolves them.
//
// Run this, then start DockForge on top of it, then measure adjacent-pixel
// contrast inside the plate with tools/pattern_contrast.ps1.
//
//   spike_t005_checker --hold-ms=20000 [--cell=8] [--x=0 --y=0 --w=2880 --h=1920]
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>

namespace {

constexpr wchar_t kClass[] = L"DockForgeSpikeChecker";

int g_cell = 8;
bool g_topmost = false;
// When flat_r >= 0 the window paints one solid colour instead of a checkerboard.
// A pair of flat backdrops (dark then light) is the differential test that proves
// the plate really samples what is behind it: a blurred plate's own colour tracks
// the backdrop, an opaque plate's does not.
int flat_r = -1;
int flat_g = -1;
int flat_b = -1;

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HDC dc = reinterpret_cast<HDC>(wp);
        if (flat_r >= 0) {
            HBRUSH brush = CreateSolidBrush(RGB(flat_r, flat_g, flat_b));
            FillRect(dc, &rc, brush);
            DeleteObject(brush);
            return 1;
        }
        // Pure black/white: maximum luminance separation, so a surviving pattern is
        // unambiguous and a blurred one averages to a flat mid-grey.
        HBRUSH light = CreateSolidBrush(RGB(255, 255, 255));
        HBRUSH dark = CreateSolidBrush(RGB(0, 0, 0));
        for (int y = 0; y < rc.bottom; y += g_cell) {
            for (int x = 0; x < rc.right; x += g_cell) {
                RECT cell{x, y, x + g_cell, y + g_cell};
                FillRect(dc, &cell, (((x / g_cell) + (y / g_cell)) % 2) ? dark : light);
            }
        }
        DeleteObject(light);
        DeleteObject(dark);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int hold_ms = 20000;
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto value = [&](const wchar_t* key) -> std::wstring {
            const std::wstring prefix = std::wstring(key) + L"=";
            return arg.rfind(prefix, 0) == 0 ? arg.substr(prefix.size()) : std::wstring{};
        };
        if (auto v = value(L"--hold-ms"); !v.empty()) hold_ms = _wtoi(v.c_str());
        else if (auto v = value(L"--cell"); !v.empty()) g_cell = _wtoi(v.c_str());
        else if (auto v = value(L"--x"); !v.empty()) x = _wtoi(v.c_str());
        else if (auto v = value(L"--y"); !v.empty()) y = _wtoi(v.c_str());
        else if (auto v = value(L"--w"); !v.empty()) w = _wtoi(v.c_str());
        else if (auto v = value(L"--h"); !v.empty()) h = _wtoi(v.c_str());
        else if (arg == L"--topmost") g_topmost = true;
        else if (auto v = value(L"--flat"); !v.empty()) {
            // --flat=r,g,b paints one colour instead of a checkerboard.
            const std::wstring csv = v;
            const size_t a = csv.find(L',');
            const size_t b = csv.find(L',', a + 1);
            if (a != std::wstring::npos && b != std::wstring::npos) {
                flat_r = _wtoi(csv.substr(0, a).c_str());
                flat_g = _wtoi(csv.substr(a + 1, b - a - 1).c_str());
                flat_b = _wtoi(csv.substr(b + 1).c_str());
            }
        }
    }
    if (g_cell < 2) g_cell = 2;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (w <= 0) w = GetSystemMetrics(SM_CXSCREEN);
    if (h <= 0) h = GetSystemMetrics(SM_CYSCREEN);

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    // --topmost exists because a full-screen app window (Settings, a game, a
    // video) covers the desktop wallpaper and turns every backdrop probe into a
    // flat colour. Measuring the frost requires the pattern to be the topmost
    // thing behind the plate.
    const DWORD ex = WS_EX_TOOLWINDOW | (g_topmost ? WS_EX_TOPMOST : 0);
    HWND hwnd = CreateWindowExW(ex, kClass, L"checker", WS_POPUP, x, y, w, h, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    if (g_topmost) {
        SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    std::printf("checker window %dx%d at %d,%d cell=%d hold=%dms\n", w, h, x, y, g_cell, hold_ms);
    std::fflush(stdout);

    // Pump messages so the window keeps painting; a bare Sleep would let DWM
    // consider it unresponsive and the checkerboard would not appear.
    const DWORD start = GetTickCount();
    MSG msg{};
    while (GetTickCount() - start < static_cast<DWORD>(hold_ms)) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(20);
    }
    DestroyWindow(hwnd);
    return 0;
}
