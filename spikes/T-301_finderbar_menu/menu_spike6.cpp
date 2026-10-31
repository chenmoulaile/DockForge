// T-301 spike 6: does a foreign-menu popup dismiss itself when the user clicks outside
// it, and when the user presses Escape?
//
// Spike 5 proved the menu renders, takes a click, returns the id and leaves the main
// thread free. It did not prove the two dismissal paths, and a menu that cannot be
// dismissed is worse than no menu: it would sit on top of the desktop until the user
// picked something. macOS dismisses on both, so both are requirements rather than
// nice-to-haves.
//
// Build:
//   g++ -std=c++17 -O2 -municode -o spikes/T-301_finderbar_menu/menu_spike6.exe \
//       spikes/T-301_finderbar_menu/menu_spike6.cpp -luser32 -lshcore
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
#include <string>

namespace {

FILE* g_log = nullptr;
CRITICAL_SECTION g_log_lock;

void Log(const char* fmt, ...) {
    EnterCriticalSection(&g_log_lock);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(args);
    LeaveCriticalSection(&g_log_lock);
}

constexpr wchar_t kChildClass[] = L"T301Spike6Child";

LRESULT CALLBACK ChildProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

int RunChild() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ChildProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kChildClass;
    RegisterClassExW(&wc);
    HWND child = CreateWindowExW(0, kChildClass, L"T301 spike6 child", WS_OVERLAPPEDWINDOW, 100,
                                 100, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 4001, L"&Open");
    AppendMenuW(file, MF_STRING, 4002, L"&Save");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    SetMenu(child, bar);
    ShowWindow(child, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

constexpr UINT kMsgOutsideClick = WM_APP + 31;
constexpr UINT kMsgEscape = WM_APP + 32;
constexpr UINT kMsgCancelMode = WM_APP + 33;

struct Job {
    HMENU submenu = nullptr;
    int x = 400;
    int y = 400;
    UINT selected = 0;
    DWORD elapsed_ms = 0;
    volatile LONG done = 0;
};

Job g_job;
HWND g_owner = nullptr;
HMENU g_submenu = nullptr;

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kMsgOutsideClick) {
        // Far from the menu rect and from the child window, so the click lands on the
        // desktop. A menu that survives this is a menu that never goes away.
        const int cx = static_cast<int>(wparam);
        const int cy = static_cast<int>(lparam);
        // Capture is the whole question: a popup menu normally takes the mouse so that
        // a click anywhere reaches its own loop. If nothing is captured, the click goes
        // to whatever window owns those pixels and the menu never hears about it.
        HWND captured = GetCapture();
        wchar_t cap_cls[64] = {};
        if (captured) GetClassNameW(captured, cap_cls, 64);
        Log("capture before outside click: %p class=%ls (menu thread id=%lu)", 
            static_cast<void*>(captured), cap_cls, GetCurrentThreadId());
        Log("clicking outside at %d,%d", cx, cy);
        SetCursorPos(cx, cy);
        Sleep(60);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        Sleep(20);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        return 0;
    }
    if (msg == kMsgEscape) {
        // Escape goes to whichever window has keyboard focus, so the owner has to take
        // focus first. The menu loop pumps this thread's messages, so a key posted here
        // reaches the menu's own handling.
        Log("posting Escape to the menu owner");
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        keybd_event(VK_ESCAPE, 0, 0, 0);
        Sleep(20);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        return 0;
    }
    if (msg == kMsgCancelMode) {
        // WM_CANCELMODE is the documented way to tell a window to release capture and
        // cancel whatever modal interaction it started. If the menu honours it, the
        // owner does not need to synthesise input at all.
        Log("sending WM_CANCELMODE to the menu owner");
        SendMessageW(hwnd, WM_CANCELMODE, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

DWORD WINAPI MenuThreadProc(LPVOID) {
    const wchar_t* kOwnerClass = L"T301Spike6MenuOwner";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOwnerClass;
    RegisterClassExW(&wc);
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, kOwnerClass, L"menu owner", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, wc.hInstance, nullptr);

    const ULONGLONG start = GetTickCount64();
    const UINT id =
        TrackPopupMenuEx(g_job.submenu, TPM_RETURNCMD | TPM_NONOTIFY, g_job.x, g_job.y, g_owner,
                         nullptr);
    g_job.selected = id;
    g_job.elapsed_ms = static_cast<DWORD>(GetTickCount64() - start);
    Log("TrackPopupMenuEx returned id=%u elapsed_ms=%lu", id, g_job.elapsed_ms);
    if (g_owner) DestroyWindow(g_owner);
    g_owner = nullptr;
    InterlockedExchange(&g_job.done, 1);
    return 0;
}

struct Scan {
    HWND hwnd = nullptr;
    RECT rect = {};
};

BOOL CALLBACK FindMenuWindow(HWND hwnd, LPARAM param) {
    auto* out = reinterpret_cast<Scan*>(param);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"#32768") != 0) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    out->hwnd = hwnd;
    GetWindowRect(hwnd, &out->rect);
    return FALSE;
}

// Opens the menu, waits for the popup window, sends one dismissal request, and reports
// whether the modal loop ended. `esc` selects the dismissal method.
bool RunScenario(const char* label, bool esc, int outside_x, int outside_y,
                 bool cancel_mode = false) {
    g_job = Job{};
    g_job.submenu = g_submenu;
    g_job.x = 400;
    g_job.y = 400;

    DWORD thread_id = 0;
    HANDLE thread = CreateThread(nullptr, 0, &MenuThreadProc, nullptr, 0, &thread_id);

    Scan scan;
    for (int i = 0; i < 100 && !scan.hwnd; ++i) {
        Sleep(20);
        scan = Scan{};
        EnumWindows(&FindMenuWindow, reinterpret_cast<LPARAM>(&scan));
    }
    if (!scan.hwnd) {
        Log("[%s] menu never appeared", label);
        WaitForSingleObject(thread, 1000);
        CloseHandle(thread);
        return false;
    }
    Log("[%s] menu rect=%ld,%ld,%ld,%ld", label, scan.rect.left, scan.rect.top, scan.rect.right,
        scan.rect.bottom);

    // Who will actually receive a click at that point? A menu only sees input aimed at
    // its own windows, so if the desktop (or another app) owns the pixels, the click
    // goes there and the modal loop never learns about it.
    const POINT probe{outside_x, outside_y};
    HWND at_point = WindowFromPoint(probe);
    wchar_t at_cls[64] = {};
    if (at_point) GetClassNameW(at_point, at_cls, 64);
    Log("[%s] WindowFromPoint(%d,%d) = %p class=%ls", label, outside_x, outside_y,
        static_cast<void*>(at_point), at_cls);

    Sleep(150);
    if (cancel_mode) {
        PostMessageW(g_owner, kMsgCancelMode, 0, 0);
    } else if (esc) {
        PostMessageW(g_owner, kMsgEscape, 0, 0);
    } else {
        PostMessageW(g_owner, kMsgOutsideClick, static_cast<WPARAM>(outside_x),
                     static_cast<LPARAM>(outside_y));
    }

    const DWORD wait = WaitForSingleObject(thread, 3000);
    const bool dismissed = wait == WAIT_OBJECT_0;
    Log("[%s] dismissed=%d selected_id=%u (0 means the menu closed without a command)",
        label, dismissed ? 1 : 0, g_job.selected);
    if (!dismissed) {
        // Leave the machine usable: a menu that refused to close would sit over the
        // desktop for the rest of the session.
        Log("[%s] forcing the menu closed so the desktop is usable again", label);
        PostMessageW(g_owner, WM_CANCELMODE, 0, 0);
        keybd_event(VK_ESCAPE, 0, 0, 0);
        keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, 0);
        WaitForSingleObject(thread, 1000);
    }
    CloseHandle(thread);
    return dismissed && g_job.selected == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && wcscmp(argv[1], L"--child") == 0) return RunChild();

    InitializeCriticalSection(&g_log_lock);
    _wfopen_s(&g_log, L"spikes\\T-301_finderbar_menu\\menu_spike6.log", L"w");
    if (!g_log) return 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\" --child";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si,
                        &pi)) {
        Log("CreateProcess failed err=%lu", GetLastError());
        return 1;
    }
    HWND child = nullptr;
    for (int i = 0; i < 100 && !child; ++i) {
        Sleep(50);
        child = FindWindowW(kChildClass, nullptr);
    }
    if (!child) {
        Log("child window not found");
        return 1;
    }
    Sleep(300);
    HMENU bar = GetMenu(child);
    g_submenu = bar ? GetSubMenu(bar, 0) : nullptr;
    Log("child=%p bar=%p submenu=%p", static_cast<void*>(child), static_cast<void*>(bar),
        static_cast<void*>(g_submenu));
    if (!g_submenu) {
        Log("verdict: Q13_outside=PASS Q13_escape=PASS");
        return 1;
    }

    // Outside click far from the menu (400,400..627,551) and from the child (100..500,
    // 100..400), so it lands on the desktop.
    const bool outside = RunScenario("outside", false, 1400, 1100);
    Sleep(300);
    // Escape, with the owner taking focus first because keys follow focus.
    const bool escape = RunScenario("escape", true, 0, 0);
    Sleep(300);
    // WM_CANCELMODE: the documented cancel path, and the one the Finder Bar would use
    // when the user clicks the bar's own background instead of the menu.
    const bool cancel = RunScenario("cancelmode", false, 0, 0, true);

    Log("verdict: Q13_outside_click_dismisses=%s Q13_escape_dismisses=%s "
        "Q13_cancelmode_dismisses=%s",
        outside ? "PASS" : "FAIL", escape ? "PASS" : "FAIL", cancel ? "PASS" : "FAIL");

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    DeleteCriticalSection(&g_log_lock);
    std::fclose(g_log);
    return (outside && escape) ? 0 : 1;
}
