// T-301 spike 8 - who owns the foreground when the menu opens, and does it matter?
//
// Spike 7 accidentally produced the answer to a different question than it asked. Its
// first scenario left Chrome in the foreground and the menu then ignored both a real
// Escape and an outside click; the later scenarios had the menu owner in the foreground
// and the menu closed on an outside click with no help at all. The variable was never
// the dismissal technique - it was foreground ownership.
//
// That matters because a menu bar is always clicked before it opens a menu, so the real
// question is: after a user click on our own window, does the process hold foreground
// strongly enough that a worker thread can open a menu and keep it? Three checks:
//
//   Q16  A real click on our window makes our process foreground.
//   Q17  From that state, a menu opened on a worker thread keeps foreground.
//   Q18  In that state, a real Escape closes the menu, and an outside click does too.
//
// If Q17/Q18 pass, finder_bar.cpp needs no polling, no hooks and no synthetic input:
// it opens the menu on a worker thread and lets Windows do the dismissal, exactly like
// a native menu bar.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellscalingapi.h>

#include <cstdarg>
#include <cstdio>

namespace {

FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    if (g_log) {
        vfprintf(g_log, fmt, args);
        fputc('\n', g_log);
        fflush(g_log);
    }
    va_end(args);
}

HMENU g_submenu = nullptr;
HWND g_owner = nullptr;
volatile LONG g_done = 0;
volatile LONG g_selected = 0;

constexpr wchar_t kBarClass[] = L"T301Spike8Bar";

// A stand-in for the Finder Bar: a real window the test clicks on, which is what grants
// the process foreground in production.
LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_LBUTTONDOWN) {
        Log("bar: WM_LBUTTONDOWN, taking the click");
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

constexpr wchar_t kOwnerClass[] = L"T301Spike8Owner";

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_CANCELMODE) {
        Log("owner: WM_CANCELMODE");
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

DWORD WINAPI MenuThreadProc(LPVOID) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOwnerClass;
    RegisterClassExW(&wc);
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, kOwnerClass, L"menu owner", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, wc.hInstance, nullptr);

    // The documented requirement for a popup menu to receive keyboard input: its owner
    // must be foreground. Whether this succeeds is exactly what Q17 measures.
    const BOOL fg_ok = SetForegroundWindow(g_owner);
    const HWND fg = GetForegroundWindow();
    Log("menu thread: SetForegroundWindow=%d foreground=%p owner=%p match=%d", fg_ok ? 1 : 0,
        static_cast<void*>(fg), static_cast<void*>(g_owner), fg == g_owner ? 1 : 0);
    SetFocus(g_owner);

    const ULONGLONG start = GetTickCount64();
    const UINT id =
        TrackPopupMenuEx(g_submenu, TPM_RETURNCMD | TPM_NONOTIFY, 400, 400, g_owner, nullptr);
    InterlockedExchange(&g_selected, static_cast<LONG>(id));
    Log("menu thread: returned id=%u elapsed_ms=%llu", id, GetTickCount64() - start);
    if (g_owner) DestroyWindow(g_owner);
    g_owner = nullptr;
    InterlockedExchange(&g_done, 1);
    return 0;
}

struct PopupScan {
    HWND hwnd = nullptr;
    RECT rect = {};
};

BOOL CALLBACK FindPopup(HWND hwnd, LPARAM param) {
    auto* out = reinterpret_cast<PopupScan*>(param);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"#32768") != 0) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    out->hwnd = hwnd;
    GetWindowRect(hwnd, &out->rect);
    return FALSE;
}

// Clicks a point with real mouse input, so the foreground rules apply as they do for a
// user rather than being short-circuited by a synthetic activation call.
void RealClick(int x, int y) {
    SetCursorPos(x, y);
    Sleep(80);
    mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
    Sleep(40);
    mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
    Sleep(80);
}

// Runs one scenario with `mode` 0 = Escape, 1 = outside click. Returns whether the menu
// closed.
bool RunScenario(const char* label, HWND bar, int bar_x, int bar_y, int mode) {
    InterlockedExchange(&g_done, 0);
    InterlockedExchange(&g_selected, 0);

    // Q16: the user clicks the bar first. This is the step production always has and the
    // spike 7 scenarios were missing.
    RealClick(bar_x, bar_y);
    const HWND fg_after_click = GetForegroundWindow();
    Log("[%s] after clicking the bar: foreground=%p bar=%p match=%d", label,
        static_cast<void*>(fg_after_click), static_cast<void*>(bar), fg_after_click == bar ? 1 : 0);

    DWORD thread_id = 0;
    HANDLE thread = CreateThread(nullptr, 0, &MenuThreadProc, nullptr, 0, &thread_id);

    PopupScan scan;
    for (int i = 0; i < 100 && !scan.hwnd; ++i) {
        Sleep(20);
        scan = PopupScan{};
        EnumWindows(&FindPopup, reinterpret_cast<LPARAM>(&scan));
    }
    if (!scan.hwnd) {
        Log("[%s] popup never appeared", label);
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
        return false;
    }
    Log("[%s] popup rect=%ld,%ld,%ld,%ld", label, scan.rect.left, scan.rect.top, scan.rect.right,
        scan.rect.bottom);

    Sleep(150);
    if (mode == 0) {
        Log("[%s] sending real Escape", label);
        keybd_event(VK_ESCAPE, 0, 0, 0);
        Sleep(30);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    } else {
        // Far from the popup rect (400,400..627,551), so the press lands on the desktop.
        Log("[%s] clicking outside the popup at 1500,1200", label);
        RealClick(1500, 1200);
    }

    const DWORD wait = WaitForSingleObject(thread, 2500);
    const bool dismissed = wait == WAIT_OBJECT_0;
    Log("[%s] dismissed=%d selected=%ld", label, dismissed ? 1 : 0, g_selected);
    if (!dismissed) {
        Log("[%s] forcing the menu closed so the desktop is usable again", label);
        if (g_owner) PostMessageW(g_owner, WM_CANCELMODE, 0, 0);
        WaitForSingleObject(thread, 1000);
    }
    CloseHandle(thread);
    return dismissed;
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_log = fopen("menu_spike8.log", "w");
    if (!g_log) return 2;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &BarProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kBarClass;
    RegisterClassExW(&wc);
    // Topmost and no-activate are what the real bar uses; the click still activates it
    // because WS_EX_NOACTIVATE is deliberately absent (spike Q7 variant 1).
    HWND bar = CreateWindowExW(WS_EX_TOPMOST, kBarClass, L"spike8 bar", WS_POPUP, 0, 0, 1200, 56,
                               nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(bar, SW_SHOWNA);
    UpdateWindow(bar);

    HMENU top = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 5001, L"&Open");
    AppendMenuW(file, MF_STRING, 5002, L"&Save");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 5003, L"E&xit");
    AppendMenuW(top, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    g_submenu = file;

    Log("bar=%p submenu=%p screen=%dx%d", static_cast<void*>(bar), static_cast<void*>(g_submenu),
        GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));

    // Click the middle of the bar, well away from the popup at 400,400.
    const int bar_x = 900;
    const int bar_y = 28;
    const bool escape = RunScenario("escape", bar, bar_x, bar_y, 0);
    Sleep(400);
    const bool outside = RunScenario("outside", bar, bar_x, bar_y, 1);

    Log("verdict: Q16_click_gives_foreground=? Q17_menu_keeps_foreground=? "
        "Q18_escape_dismisses=%s Q18_outside_click_dismisses=%s",
        escape ? "PASS" : "FAIL", outside ? "PASS" : "FAIL");
    fclose(g_log);
    if (bar) DestroyWindow(bar);
    return (escape && outside) ? 0 : 1;
}
