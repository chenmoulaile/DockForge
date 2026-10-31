// T-301 spike 7 - can the bar dismiss its own popup menu?
//
// Spike 6 established that a tracked menu ignores a click that lands on another
// process's window, even while the menu holds capture. A menu that cannot be
// dismissed is worse than no menu, so the bar has to close it itself. Two candidates:
//
//   Q14  Real keyboard Escape, with the menu owner taking focus on the menu thread.
//        Spike 6 only proved PostMessage(SynthesisedKey) works, which no user can do.
//   Q15  Poll GetAsyncKeyState + the popup window's rect, then cancel from the main
//        thread. This is what the bar can actually run every frame: no hooks, and the
//        cost is two cheap calls.
//
// The verdict decides how finder_bar.cpp dismisses menus.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <shellscalingapi.h>

#include <cstdio>
#include <cstring>

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

constexpr UINT kMsgCancel = WM_APP + 41;

HMENU g_submenu = nullptr;
HWND g_owner = nullptr;
volatile LONG g_done = 0;
volatile LONG g_selected = 0;

// ---- the target application, so the forwarded command can be observed ----------
constexpr wchar_t kChildClass[] = L"T301Spike7Child";
constexpr UINT kMsgChildCommand = WM_APP + 42;
HWND g_child = nullptr;
volatile LONG g_child_last_command = 0;

LRESULT CALLBACK ChildProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COMMAND) {
        InterlockedExchange(&g_child_last_command, static_cast<LONG>(LOWORD(wparam)));
        return 0;
    }
    if (msg == kMsgChildCommand) {
        InterlockedExchange(&g_child_last_command, static_cast<LONG>(wparam));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

HWND CreateChild() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ChildProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kChildClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, kChildClass, L"spike7 child", WS_OVERLAPPEDWINDOW, 100, 100, 500,
                                300, nullptr, nullptr, wc.hInstance, nullptr);
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 4001, L"&Open");
    AppendMenuW(file, MF_STRING, 4002, L"&Save");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 4003, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    SetMenu(hwnd, bar);
    g_submenu = file;
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

// ---- the menu owner, created on the menu thread --------------------------------
constexpr wchar_t kOwnerClass[] = L"T301Spike7Owner";

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kMsgCancel) {
        // The menu loop dispatches messages for this thread, so a posted message to a
        // window we own is the only way to reach it while it is blocked. DefWindowProc
        // ends the tracked menu on WM_CANCELMODE.
        Log("menu thread: cancel requested, sending WM_CANCELMODE");
        SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
        return 0;
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

    // Q14: a menu bar belongs to the app in front, so the bar has to be able to take
    // the keyboard while its menu is open. Focus has to sit on a window owned by the
    // thread running the menu loop, which is this one.
    SetForegroundWindow(g_owner);
    SetFocus(g_owner);

    const ULONGLONG start = GetTickCount64();
    const UINT id =
        TrackPopupMenuEx(g_submenu, TPM_RETURNCMD | TPM_NONOTIFY, 400, 400, g_owner, nullptr);
    InterlockedExchange(&g_selected, static_cast<LONG>(id));
    Log("menu thread: TrackPopupMenuEx returned id=%u elapsed_ms=%llu", id,
        GetTickCount64() - start);
    if (g_owner) DestroyWindow(g_owner);
    g_owner = nullptr;
    InterlockedExchange(&g_done, 1);
    return 0;
}

// ---- popup window discovery ---------------------------------------------------
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

// Runs one dismissal scenario. `mode` 0 = real Escape, 1 = poll + cancel, 2 = outside
// click with no cancellation (the control, to prove the click really is invisible).
bool RunScenario(const char* label, int mode) {
    InterlockedExchange(&g_done, 0);
    InterlockedExchange(&g_selected, 0);

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

    // The menu thread takes focus; confirm it, because a foreground change is what a
    // real menu bar relies on for keyboard input.
    HWND fg = GetForegroundWindow();
    wchar_t fg_cls[64] = {};
    if (fg) GetClassNameW(fg, fg_cls, 64);
    Log("[%s] foreground=%p class=%ls owner=%p", label, static_cast<void*>(fg), fg_cls,
        static_cast<void*>(g_owner));

    Sleep(120);
    if (mode == 0) {
        // Real keystrokes, not a posted message. This is the only form a user produces.
        Log("[%s] sending real Escape via keybd_event", label);
        keybd_event(VK_ESCAPE, 0, 0, 0);
        Sleep(20);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
    } else {
        // The click lands far from the popup, on the desktop or another app.
        const int cx = 1500;
        const int cy = 1200;
        Log("[%s] clicking outside at %d,%d", label, cx, cy);
        SetCursorPos(cx, cy);
        Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        // Q15: this is exactly what the bar can do each frame while a menu is open.
        // GetAsyncKeyState reports the physical button; the popup rect says whether the
        // press belongs to the menu. A press outside both means "dismiss".
        const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const POINT pt = [] {
            POINT p{};
            GetCursorPos(&p);
            return p;
        }();
        const bool in_popup = PtInRect(&scan.rect, pt) != 0;
        Log("[%s] poll: button_down=%d cursor=%ld,%ld in_popup=%d", label, down ? 1 : 0, pt.x,
            pt.y, in_popup ? 1 : 0);
        Sleep(20);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);

        if (mode == 1 && down && !in_popup && g_owner) {
            Log("[%s] outside press detected, cancelling the menu", label);
            PostMessageW(g_owner, kMsgCancel, 0, 0);
        }
    }

    const DWORD wait = WaitForSingleObject(thread, 2500);
    const bool dismissed = wait == WAIT_OBJECT_0;
    Log("[%s] dismissed=%d selected=%ld", label, dismissed ? 1 : 0, g_selected);
    if (!dismissed) {
        Log("[%s] forcing the menu closed so the desktop is usable again", label);
        if (g_owner) PostMessageW(g_owner, kMsgCancel, 0, 0);
        WaitForSingleObject(thread, 1000);
    }
    CloseHandle(thread);
    return dismissed;
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_log = fopen("menu_spike7.log", "w");
    if (!g_log) return 2;

    g_child = CreateChild();
    Log("child=%p submenu=%p", static_cast<void*>(g_child), static_cast<void*>(g_submenu));

    // Q14 first: if real Escape works, the bar can rely on the keyboard and skip polling.
    const bool escape = RunScenario("escape", 0);
    Sleep(400);

    // Q15: outside press, polled. Mode 2 is the control - same click, no cancellation.
    const bool control = RunScenario("control", 2);
    Sleep(400);
    const bool polled = RunScenario("poll", 1);

    Log("verdict: Q14_real_escape_dismisses=%s Q15_control_stays_open=%s "
        "Q15_poll_cancel_dismisses=%s",
        escape ? "PASS" : "FAIL", control ? "FAIL" : "PASS", polled ? "PASS" : "FAIL");
    fclose(g_log);
    if (g_child) DestroyWindow(g_child);
    return (escape && !control && polled) ? 0 : 1;
}
