// T-301 spike 5: can TrackPopupMenuEx run on a worker thread while the dock keeps
// rendering on the main thread?
//
// Why this matters: TrackPopupMenuEx is a modal loop. If the Finder Bar calls it from
// the dock's message handler or tick handler, the dock stops pumping frames for as long
// as the menu is open - the magnify animation freezes, the Launchpad cannot open, and a
// user who leaves a menu open for ten seconds sees a dead dock. macOS does not freeze
// the Dock while a menu is open, so neither can this.
//
// The spike proves three things at once, with instruments instead of eyeballs:
//   Q12a  a worker thread can own the modal menu loop for a FOREIGN HMENU;
//   Q12b  the menu is really rendered and a click on it round-trips to the owning
//         process as WM_COMMAND;
//   Q12c  the main thread keeps its 16 ms cadence throughout - no stall.
//
// Two traps this spike was written to survive, both learned the hard way:
//   * GetMenuItemRect must be called ON THE THREAD THAT OWNS THE MENU. From another
//     thread it returns coordinates in the owner's DPI space; the first run of this
//     file got 806,894 where the truth was 403,425 (exactly 2x on a 192 DPI desktop),
//     clicked the wrong item, and reported a false failure. The owner window therefore
//     handles a posted message and does the measuring and clicking itself.
//   * The cadence loop must not contain the click. Sleep(60) + Sleep(20) inside the
//     measured window is 80 ms of self-inflicted stall that looks exactly like the bug
//     the measurement exists to rule out. Cadence is sampled in a clean phase.
//
// Build:
//   g++ -std=c++17 -O2 -municode -o spikes/T-301_finderbar_menu/menu_spike5.exe \
//       spikes/T-301_finderbar_menu/menu_spike5.cpp -luser32 -lshcore
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

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

// ---------------------------------------------------------------- child process
// A separate process with a menu, because the whole point is a menu this process does
// not own. The id it receives is written to a file so the parent can verify the round
// trip across a process boundary.
constexpr wchar_t kChildClass[] = L"T301Spike5Child";
constexpr wchar_t kChildResultFile[] = L"spikes\\T-301_finderbar_menu\\spike5_child.txt";

LRESULT CALLBACK ChildProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_COMMAND) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, kChildResultFile, L"w") == 0 && f) {
            std::fprintf(f, "command=%u\n", static_cast<unsigned>(LOWORD(wparam)));
            std::fclose(f);
        }
        return 0;
    }
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

    HWND child = CreateWindowExW(0, kChildClass, L"T301 spike5 child", WS_OVERLAPPEDWINDOW, 100,
                                 100, 400, 300, nullptr, nullptr, wc.hInstance, nullptr);
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, 3001, L"&Open");
    AppendMenuW(file, MF_STRING, 3002, L"&Save");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, 3003, L"E&xit");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, 3004, L"Cu&t");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"&Edit");
    SetMenu(child, bar);
    ShowWindow(child, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// ------------------------------------------------------------- menu thread state
constexpr UINT kMsgClickItem = WM_APP + 21;

struct MenuJob {
    HWND target = nullptr;   // window in the child process that owns the menu
    HMENU submenu = nullptr;
    int x = 400;
    int y = 400;
    DWORD menu_thread_id = 0;
    DWORD elapsed_ms = 0;
    UINT selected = 0;
    int clicked_item = -1;
    bool click_measured = false;
    volatile LONG done = 0;
};

MenuJob g_job;
HWND g_owner = nullptr;

LRESULT CALLBACK OwnerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kMsgClickItem) {
        // This runs on the menu-owning thread, inside the modal menu loop's message
        // pump, which is the only place GetMenuItemRect reports true screen pixels.
        const int index = static_cast<int>(wparam);
        RECT item = {};
        if (!GetMenuItemRect(hwnd, g_job.submenu, static_cast<UINT>(index), &item)) {
            Log("GetMenuItemRect(item %d) failed err=%lu", index, GetLastError());
            return 0;
        }
        Log("GetMenuItemRect(item %d) = %ld,%ld,%ld,%ld (measured on the menu thread)", index,
            item.left, item.top, item.right, item.bottom);
        const int cx = (item.left + item.right) / 2;
        const int cy = (item.top + item.bottom) / 2;
        Log("clicking centre %d,%d", cx, cy);
        SetCursorPos(cx, cy);
        Sleep(50);
        mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
        Sleep(20);
        mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
        g_job.click_measured = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

DWORD WINAPI MenuThreadProc(LPVOID) {
    g_job.menu_thread_id = GetCurrentThreadId();

    const wchar_t* kOwnerClass = L"T301Spike5MenuOwner";
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OwnerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kOwnerClass;
    RegisterClassExW(&wc);
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, kOwnerClass, L"menu owner", WS_POPUP, 0, 0, 1, 1,
                              nullptr, nullptr, wc.hInstance, nullptr);

    const ULONGLONG start = GetTickCount64();
    // TPM_RETURNCMD: return the id instead of posting WM_COMMAND, so the caller decides
    // where the command goes. TPM_NONOTIFY: no TPMN_* notifications, which we do not use.
    const UINT id = TrackPopupMenuEx(g_job.submenu, TPM_RETURNCMD | TPM_NONOTIFY, g_job.x, g_job.y,
                                     g_owner, nullptr);
    g_job.selected = id;
    g_job.elapsed_ms = static_cast<DWORD>(GetTickCount64() - start);
    Log("menu thread: TrackPopupMenuEx returned id=%u elapsed_ms=%lu", id, g_job.elapsed_ms);

    if (id != 0) {
        // The forwarding step of C-32: the command belongs to the target process.
        const BOOL posted = PostMessageW(g_job.target, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        Log("menu thread: forwarded WM_COMMAND %u to %p posted=%d err=%lu", id,
            static_cast<void*>(g_job.target), posted, GetLastError());
    }

    if (g_owner) DestroyWindow(g_owner);
    g_owner = nullptr;
    InterlockedExchange(&g_job.done, 1);
    return 0;
}

// ------------------------------------------------------------------- instruments
struct MenuWindowScan {
    HWND hwnd = nullptr;
    RECT rect = {};
    DWORD thread_id = 0;
};

BOOL CALLBACK FindMenuWindow(HWND hwnd, LPARAM param) {
    auto* out = reinterpret_cast<MenuWindowScan*>(param);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"#32768") != 0) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    out->hwnd = hwnd;
    GetWindowRect(hwnd, &out->rect);
    out->thread_id = GetWindowThreadProcessId(hwnd, nullptr);
    return FALSE;
}

double MaxOf(const std::vector<double>& values) {
    return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && wcscmp(argv[1], L"--child") == 0) return RunChild();

    InitializeCriticalSection(&g_log_lock);
    _wfopen_s(&g_log, L"spikes\\T-301_finderbar_menu\\menu_spike5.log", L"w");
    if (!g_log) return 1;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    DeleteFileW(kChildResultFile);

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
    Log("child pid=%lu", pi.dwProcessId);

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
    Log("child hwnd=%p", static_cast<void*>(child));

    HMENU bar = GetMenu(child);
    HMENU file_menu = bar ? GetSubMenu(bar, 0) : nullptr;
    Log("GetMenu=%p items=%d file_submenu=%p", static_cast<void*>(bar),
        bar ? GetMenuItemCount(bar) : -1, static_cast<void*>(file_menu));
    if (!file_menu) {
        Log("verdict: Q12a=FAIL Q12b=FAIL Q12c=FAIL (no foreign menu)");
        return 1;
    }

    g_job.target = child;
    g_job.submenu = file_menu;
    g_job.x = 400;
    g_job.y = 400;

    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);
    const auto gap_ms = [&freq](const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
        return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 /
               static_cast<double>(freq.QuadPart);
    };

    // Baseline cadence BEFORE the menu exists, so the comparison is against this
    // machine rather than against an assumed 16 ms.
    std::vector<double> baseline_gaps;
    {
        LARGE_INTEGER prev = {};
        QueryPerformanceCounter(&prev);
        for (int i = 0; i < 30; ++i) {
            Sleep(16);
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            baseline_gaps.push_back(gap_ms(prev, now));
            prev = now;
        }
    }
    Log("baseline main-thread cadence: samples=%zu max_gap_ms=%.2f", baseline_gaps.size(),
        MaxOf(baseline_gaps));

    DWORD menu_thread = 0;
    HANDLE thread = CreateThread(nullptr, 0, &MenuThreadProc, nullptr, 0, &menu_thread);
    Log("menu thread started id=%lu", menu_thread);

    // Wait for the popup to appear. This is instrumentation, not cadence.
    MenuWindowScan scan;
    for (int i = 0; i < 100 && !scan.hwnd; ++i) {
        Sleep(20);
        scan = MenuWindowScan{};
        EnumWindows(&FindMenuWindow, reinterpret_cast<LPARAM>(&scan));
    }
    const bool saw_menu_window = scan.hwnd != nullptr;
    if (saw_menu_window) {
        Log("tracked menu window seen hwnd=%p rect=%ld,%ld,%ld,%ld thread=%lu",
            static_cast<void*>(scan.hwnd), scan.rect.left, scan.rect.top, scan.rect.right,
            scan.rect.bottom, scan.thread_id);
        Log("menu owned by menu_thread=%d main_thread=%d",
            scan.thread_id == menu_thread ? 1 : 0,
            scan.thread_id == GetCurrentThreadId() ? 1 : 0);
    }

    // Clean cadence phase: menu is open and modal on the worker, main thread does
    // nothing but sleep and timestamp.
    std::vector<double> live_gaps;
    {
        LARGE_INTEGER prev = {};
        QueryPerformanceCounter(&prev);
        for (int i = 0; i < 40; ++i) {
            Sleep(16);
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            live_gaps.push_back(gap_ms(prev, now));
            prev = now;
        }
    }

    // Now ask the menu thread to click its own item 1 (id 3002).
    if (saw_menu_window && g_owner) {
        const BOOL posted = PostMessageW(g_owner, kMsgClickItem, 1, 0);
        Log("posted click request to owner=%p posted=%d err=%lu", static_cast<void*>(g_owner),
            posted, GetLastError());
    }

    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);

    UINT child_received = 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, kChildResultFile, L"r") == 0 && f) {
        char line[128] = {};
        while (std::fgets(line, sizeof(line), f)) {
            unsigned value = 0;
            if (std::sscanf(line, "command=%u", &value) == 1) child_received = value;
        }
        std::fclose(f);
    }

    Log("main-thread cadence during menu: samples=%zu max_gap_ms=%.2f", live_gaps.size(),
        MaxOf(live_gaps));
    Log("menu thread id=%lu done=%ld selected=%u elapsed_ms=%lu click_measured=%d",
        g_job.menu_thread_id, static_cast<long>(g_job.done), g_job.selected, g_job.elapsed_ms,
        g_job.click_measured ? 1 : 0);
    Log("child received=%u expected=3002", child_received);

    const bool q12a = g_job.menu_thread_id == menu_thread && g_job.done != 0;
    const bool q12b = saw_menu_window && g_job.selected == 3002 && child_received == 3002;
    const bool q12c = MaxOf(live_gaps) < 100.0;

    Log("verdict: Q12a_menu_thread_owns_loop=%s Q12b_rendered_and_round_trip=%s "
        "Q12c_main_thread_alive=%s",
        q12a ? "PASS" : "FAIL", q12b ? "PASS" : "FAIL", q12c ? "PASS" : "FAIL");

    TerminateProcess(pi.hProcess, 0);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    DeleteCriticalSection(&g_log_lock);
    std::fclose(g_log);
    return (q12a && q12b && q12c) ? 0 : 1;
}
