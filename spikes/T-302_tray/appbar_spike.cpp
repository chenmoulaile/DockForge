// T-302b spike: can DockForge reserve the top strip like the taskbar does?
//
// The Finder Bar wants to be permanently pinned (the user asked for exactly that),
// but a pinned bar covers the top 28dip of every maximized window. Q8 in
// spikes/T-301_finderbar_menu already proved SPI_SETWORKAREA cannot fix this: the
// call returns success and explorer immediately rewrites the work area.
//
// There is a second, completely different mechanism that was never tried: the shell
// AppBar protocol (SHAppBarMessage). The taskbar itself is an appbar, which is why a
// maximized window stops above it. An appbar tells the shell "reserve this edge" and
// the shell then adjusts the work area AND repositions maximized windows itself - no
// SPI_SETWORKAREA involved.
//
// If this works, the bar can be pinned by default with no covering problem, which is
// the macOS behaviour. If it does not, the bar has to auto-hide and the honest
// default stays auto_hide=true.
//
// Questions:
//   Q10  Does ABM_NEW register us with the shell?
//   Q11  Does ABM_SETPOS actually move the work area? (the whole point)
//   Q12  Does a maximized window respect the reserved strip afterwards?
//   Q13  Does ABM_REMOVE restore the original work area exactly?
//   Q14  Does the shell send ABN_POSCHANGED / ABN_FULLSCREENAPP callbacks?
//
// Build:
//   g++ -std=c++17 -O2 -o appbar_spike.exe appbar_spike.cpp -luser32 -lshell32 -lgdi32

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>

namespace {

FILE* g_log = nullptr;

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fputs(line, stdout);
    std::fflush(stdout);
    if (g_log != nullptr) {
        std::fputs(line, g_log);
        std::fputc('\n', g_log);
        std::fflush(g_log);
    }
}

constexpr UINT kCallbackMessage = WM_APP + 90;
constexpr UINT_PTR kAutoHideTimerId = 9;

HWND g_bar = nullptr;
RECT g_original_work{};
RECT g_reserved_work{};
bool g_fullscreen_app = false;
int g_pos_changed = 0;

// The strip height the Finder Bar would reserve: 28 DIP at the host's 192 DPI.
int BarHeightPx() {
    return MulDiv(28, GetDeviceCaps(GetDC(nullptr), LOGPIXELSY), 96);
}

void LogWorkArea(const char* tag) {
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    Log("  [%s] work area = %ld,%ld,%ld,%ld (%ldx%ld)", tag, work.left, work.top, work.right, work.bottom,
        work.right - work.left, work.bottom - work.top);
}

// Maximize a real application window and report where it landed. This is the only
// assertion that matters: a reserved work area that maximized windows ignore is
// worthless. The first run used the taskbar as the probe window, which lives at the
// bottom of the screen and therefore proves nothing - a normal overlapped window is
// what a user actually maximizes.
RECT MeasureMaximizedWindow(const char* tag) {
    static HWND probe = nullptr;
    if (probe == nullptr) {
        probe = CreateWindowExW(WS_EX_APPWINDOW, L"AppBarSpikeWindow", L"maximize probe", WS_OVERLAPPEDWINDOW,
                                200, 300, 640, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (probe == nullptr) {
            Log("  [%s] probe window creation failed err=%lu", tag, GetLastError());
            return RECT{};
        }
        ShowWindow(probe, SW_SHOWNOACTIVATE);
        UpdateWindow(probe);
        Sleep(120);
    }
    RECT before{};
    GetWindowRect(probe, &before);
    ShowWindow(probe, SW_MAXIMIZE);
    Sleep(260);
    RECT after{};
    GetWindowRect(probe, &after);
    ShowWindow(probe, SW_RESTORE);
    Sleep(140);
    Log("  [%s] maximized rect = %ld,%ld,%ld,%ld (%ldx%ld)", tag, after.left, after.top, after.right,
        after.bottom, after.right - after.left, after.bottom - after.top);
    return after;
}

void RegisterBar() {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_bar;
    data.uCallbackMessage = kCallbackMessage;
    const UINT_PTR registered = SHAppBarMessage(ABM_NEW, &data);
    Log("[Q10] ABM_NEW -> %llu err=%lu", static_cast<unsigned long long>(registered), GetLastError());

    // ABM_QUERYPOS first, then ABM_SETPOS with the answer. That is the documented
    // order and the shell can shrink the requested rect during the query.
    APPBARDATA query{};
    query.cbSize = sizeof(query);
    query.hWnd = g_bar;
    query.uEdge = ABE_TOP;
    query.rc = g_original_work;
    query.rc.bottom = g_original_work.top + BarHeightPx();
    SHAppBarMessage(ABM_QUERYPOS, &query);
    Log("[Q10] ABM_QUERYPOS -> %ld,%ld,%ld,%ld", query.rc.left, query.rc.top, query.rc.right,
        query.rc.bottom);

    APPBARDATA set{};
    set.cbSize = sizeof(set);
    set.hWnd = g_bar;
    set.uEdge = ABE_TOP;
    set.rc = query.rc;
    const UINT_PTR positioned = SHAppBarMessage(ABM_SETPOS, &set);
    Log("[Q10] ABM_SETPOS -> %llu rect=%ld,%ld,%ld,%ld", static_cast<unsigned long long>(positioned),
        set.rc.left, set.rc.top, set.rc.right, set.rc.bottom);

    if (positioned != FALSE) {
        g_reserved_work = g_original_work;
        g_reserved_work.top = set.rc.bottom;
        SetWindowPos(g_bar, HWND_TOPMOST, set.rc.left, set.rc.top, set.rc.right - set.rc.left,
                     set.rc.bottom - set.rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void RemoveBar() {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_bar;
    const UINT_PTR removed = SHAppBarMessage(ABM_REMOVE, &data);
    Log("[Q13] ABM_REMOVE -> %llu err=%lu", static_cast<unsigned long long>(removed), GetLastError());
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kCallbackMessage) {
        switch (wp) {
            case ABN_POSCHANGED:
                ++g_pos_changed;
                Log("  [callback] ABN_POSCHANGED");
                break;
            case ABN_FULLSCREENAPP:
                g_fullscreen_app = lp != 0;
                Log("  [callback] ABN_FULLSCREENAPP active=%d", lp != 0 ? 1 : 0);
                break;
            case ABN_STATECHANGE:
                Log("  [callback] ABN_STATECHANGE");
                break;
            default:
                Log("  [callback] unknown %u", static_cast<unsigned>(wp));
                break;
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

void Pump(int milliseconds) {
    const DWORD deadline = GetTickCount() + static_cast<DWORD>(milliseconds);
    MSG msg{};
    while (GetTickCount() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

}  // namespace

int main() {
    g_log = std::fopen("appbar_spike.log", "w");
    Log("appbar spike: can the top strip be reserved?");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Log("bar height = %d px (28 dip at %d dpi)", BarHeightPx(), GetDeviceCaps(GetDC(nullptr), LOGPIXELSY));

    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_original_work, 0);
    LogWorkArea("original");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"AppBarSpikeWindow";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("RegisterClassExW failed err=%lu", GetLastError());
        return 1;
    }
    g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"AppBarSpikeWindow", L"appbar spike", WS_POPUP,
                            0, 0, 100, BarHeightPx(), nullptr, nullptr, wc.hInstance, nullptr);
    if (g_bar == nullptr) {
        Log("CreateWindowExW failed err=%lu", GetLastError());
        return 1;
    }
    ShowWindow(g_bar, SW_SHOWNA);

    const RECT before_max = MeasureMaximizedWindow("before reserve");

    RegisterBar();
    Pump(400);
    LogWorkArea("after reserve");

    // Q11: the work area is the shell's own answer. If it did not move, nothing
    // else in this spike can work.
    RECT observed{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &observed, 0);
    const bool work_moved = observed.top >= BarHeightPx() - 2;

    const RECT after_max = MeasureMaximizedWindow("after reserve");
    // Q12: a maximized window has to start below where it started before. Windows
    // draws a maximized window a shadow border's width outside the work area (13px
    // here), so the absolute top is negative or near zero and comparing it against
    // the strip height directly would be wrong. The honest assertion is that
    // maximizing moved the window down by roughly the reserved height.
    const int shift = after_max.top - before_max.top;
    const int reserved = static_cast<int>(observed.top - g_original_work.top);
    const bool max_respects = reserved > 0 && shift >= reserved - 16;

    RemoveBar();
    Pump(400);
    LogWorkArea("after remove");
    RECT restored{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &restored, 0);
    const bool work_restored = restored.top == g_original_work.top && restored.bottom == g_original_work.bottom &&
                               restored.left == g_original_work.left && restored.right == g_original_work.right;

    Log("[Q11] work_moved=%d reserved_px=%d", work_moved ? 1 : 0, reserved);
    Log("[Q12] before_top=%ld after_top=%ld shift=%d max_respects=%d", before_max.top, after_max.top, shift,
        max_respects ? 1 : 0);
    Log("[Q13] work_restored=%d", work_restored ? 1 : 0);
    Log("[Q14] pos_changed=%d fullscreen_seen=%d", g_pos_changed, g_fullscreen_app ? 1 : 0);

    Log("verdict: Q10_appbar=%s Q11_workarea=%s Q12_maximize=%s Q13_restore=%s",
        work_moved ? "PASS" : "FAIL", work_moved ? "PASS" : "FAIL", max_respects ? "PASS" : "FAIL",
        work_restored ? "PASS" : "FAIL");

    DestroyWindow(g_bar);
    if (g_log != nullptr) std::fclose(g_log);
    return 0;
}
