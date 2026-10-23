// DockForge T-011 spike: window enumeration, activation, minimize.
//
// Three questions the dock click path depends on, none of which can be answered
// from documentation alone on this machine:
//
//   1. Which windows actually count as "the app's window"? EnumWindows returns
//      cloaked, owned, and tool windows that a user never sees. Clicking an icon
//      and landing on a hidden owner window would minimize nothing.
//   2. Does SetForegroundWindow work from a background process? Since Vista it is
//      gated by the foreground lock, and the answer decides whether the dock needs
//      the AttachThreadInput or the synthetic-input workaround.
//   3. For packaged apps, is the visible window owned by the real process or by
//      ApplicationFrameHost.exe? If the latter, mapping PID -> app is wrong.
//
// Modes: enum | activate | minimize | restore | all
#include <windows.h>
#include <dwmapi.h>
#include <appmodel.h>
#include <psapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string ToUtf8(const wchar_t* text) {
    if (text == nullptr) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

struct WindowRow {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring title;
    std::wstring cls;
    bool visible = false;
    bool cloaked = false;
    bool owned = false;
    bool tool = false;
    bool app_window = false;
};

std::vector<WindowRow> g_rows;

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM) {
    WindowRow row;
    row.hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &row.pid);

    wchar_t buffer[512]{};
    GetWindowTextW(hwnd, buffer, 511);
    row.title = buffer;
    GetClassNameW(hwnd, buffer, 511);
    row.cls = buffer;

    row.visible = IsWindowVisible(hwnd) != FALSE;

    // DWM cloaking is how the shell hides windows on other virtual desktops and
    // how packaged apps park their splash windows. IsWindowVisible still reports
    // true for those, so it cannot be used alone.
    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
        row.cloaked = cloaked != 0;
    }

    row.owned = GetWindow(hwnd, GW_OWNER) != nullptr;
    row.tool = (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0;

    // The heuristic every taskbar uses: a real app window is visible, unowned,
    // not a tool window, not cloaked, and has a title. An untitled window is
    // almost always a message-only or helper window.
    row.app_window = row.visible && !row.owned && !row.tool && !row.cloaked &&
                     !row.title.empty();

    g_rows.push_back(row);
    return TRUE;
}

void EnumerateAll() {
    g_rows.clear();
    EnumWindows(EnumProc, 0);
}

// An app's process id can own more than one window; the dock wants the one the
// user last touched, which is the one Windows considers the main window.
HWND MainWindowForPid(DWORD pid) {
    for (const auto& row : g_rows) {
        if (row.pid == pid && row.app_window) return row.hwnd;
    }
    return nullptr;
}

HWND FindByTitle(const std::wstring& needle) {
    for (const auto& row : g_rows) {
        if (row.app_window && row.title.find(needle) != std::wstring::npos) return row.hwnd;
    }
    return nullptr;
}

void PrintRow(const WindowRow& row) {
    std::printf("  hwnd=%p pid=%lu visible=%d cloaked=%d owned=%d tool=%d app=%d cls=%s title=%s\n",
                static_cast<void*>(row.hwnd), row.pid, row.visible ? 1 : 0, row.cloaked ? 1 : 0,
                row.owned ? 1 : 0, row.tool ? 1 : 0, row.app_window ? 1 : 0,
                ToUtf8(row.cls.c_str()).c_str(), ToUtf8(row.title.c_str()).c_str());
}

int ModeEnum() {
    EnumerateAll();
    std::size_t app_count = 0;
    std::printf("total_windows=%zu\n", g_rows.size());
    for (const auto& row : g_rows) {
        if (row.app_window) ++app_count;
    }
    std::printf("app_windows=%zu\n", app_count);

    std::printf("--- rejected samples ---\n");
    int shown = 0;
    for (const auto& row : g_rows) {
        if (row.app_window || shown >= 12) continue;
        PrintRow(row);
        ++shown;
    }
    std::printf("--- accepted ---\n");
    for (const auto& row : g_rows) {
        if (row.app_window) PrintRow(row);
    }
    return 0;
}

// SetForegroundWindow is refused when the calling process is not already the
// foreground process. This reports whether the call took effect, and if not,
// whether the AttachThreadInput trick changes the answer.
int ModeActivate(const std::wstring& needle) {
    EnumerateAll();
    HWND target = FindByTitle(needle);
    if (target == nullptr) {
        std::printf("target=%s not_found\n", ToUtf8(needle.c_str()).c_str());
        return 2;
    }
    std::printf("target=%p title=%s\n", static_cast<void*>(target),
                ToUtf8([&] {
                    wchar_t b[512]{};
                    GetWindowTextW(target, b, 511);
                    return std::wstring(b);
                }().c_str()).c_str());

    const HWND before = GetForegroundWindow();
    std::printf("foreground_before=%p\n", static_cast<void*>(before));

    ShowWindow(target, SW_RESTORE);
    const BOOL ok = SetForegroundWindow(target);
    Sleep(200);
    const HWND after = GetForegroundWindow();
    std::printf("set_foreground_window=%d foreground_after=%p\n", ok ? 1 : 0,
                static_cast<void*>(after));
    std::printf("verdict=%s\n", after == target ? "ACTIVATED" : "FOREGROUND_REFUSED");

    if (after != target) {
        // AttachThreadInput shares the input state so the foreground lock sees the
        // call as coming from the foreground thread. It is the documented-ish
        // workaround; this checks whether it actually works here.
        const DWORD target_tid = GetWindowThreadProcessId(target, nullptr);
        const DWORD our_tid = GetCurrentThreadId();
        const DWORD fg_tid = GetWindowThreadProcessId(before, nullptr);
        std::printf("attach: target_tid=%lu fg_tid=%lu our_tid=%lu\n", target_tid, fg_tid,
                    our_tid);
        BOOL attached = FALSE;
        if (fg_tid != 0 && fg_tid != our_tid) {
            attached = AttachThreadInput(our_tid, fg_tid, TRUE);
        }
        const BOOL ok2 = SetForegroundWindow(target);
        if (attached) AttachThreadInput(our_tid, fg_tid, FALSE);
        Sleep(200);
        const HWND after2 = GetForegroundWindow();
        std::printf("attach_thread_input=%d set_fg_again=%d foreground=%p\n",
                    attached ? 1 : 0, ok2 ? 1 : 0, static_cast<void*>(after2));
        std::printf("verdict_attach=%s\n",
                    after2 == target ? "ACTIVATED" : "STILL_REFUSED");
    }
    return 0;
}

int ModeMinimize(const std::wstring& needle) {
    EnumerateAll();
    HWND target = FindByTitle(needle);
    if (target == nullptr) {
        std::printf("target=%s not_found\n", ToUtf8(needle.c_str()).c_str());
        return 2;
    }
    std::printf("minimizing %p\n", static_cast<void*>(target));
    const BOOL ok = ShowWindow(target, SW_MINIMIZE);
    Sleep(150);
    std::printf("show_window=%d is_iconic=%d\n", ok ? 1 : 0,
                IsIconic(target) ? 1 : 0);
    std::printf("verdict=%s\n", IsIconic(target) ? "MINIMIZED" : "NOT_MINIMIZED");
    return 0;
}

int ModeRestore(const std::wstring& needle) {
    EnumerateAll();
    HWND target = FindByTitle(needle);
    if (target == nullptr) {
        std::printf("target=%s not_found\n", ToUtf8(needle.c_str()).c_str());
        return 2;
    }
    ShowWindow(target, SW_RESTORE);
    Sleep(150);
    std::printf("is_iconic=%d\n", IsIconic(target) ? 1 : 0);
    std::printf("verdict=%s\n", IsIconic(target) ? "STILL_MINIMIZED" : "RESTORED");
    return 0;
}

// Packaged apps: the visible window may belong to ApplicationFrameHost.exe while
// the real app runs in a child process. This reports both, so the dock knows
// whether a PID match is enough.
// Packaged apps: the visible ApplicationFrameWindow belongs to
// ApplicationFrameHost.exe, not to the app. This asks Windows for the real owner by
// AUMID, which is what a dock needs to match an icon to a running window.
int ModeAumid() {
    EnumerateAll();
    std::printf("--- per-window AUMID ---\n");
    int matched = 0;
    for (const auto& row : g_rows) {
        if (!row.app_window) continue;
        DWORD pid = row.pid;
        wchar_t aumid[256]{};
        UINT32 aumid_len = 256;
        LONG hr = GetApplicationUserModelId(
            [&] {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                return h;
            }(),
            &aumid_len, aumid);
        // The handle above leaks by design in a throwaway spike; report what matters.
        wchar_t exe[MAX_PATH]{};
        DWORD exe_len = MAX_PATH;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc != nullptr) {
            QueryFullProcessImageNameW(proc, 0, exe, &exe_len);
            CloseHandle(proc);
        }
        if (hr == ERROR_SUCCESS) ++matched;
        std::printf("  hwnd=%p pid=%lu aumid_hr=0x%08lX aumid=%s exe=%s title=%s\n",
                    static_cast<void*>(row.hwnd), pid, static_cast<unsigned long>(hr),
                    ToUtf8(aumid).c_str(), ToUtf8(exe).c_str(),
                    ToUtf8(row.title.c_str()).c_str());
    }
    std::printf("aumid_matched=%d\n", matched);
    return 0;
}

// Enumerate every process that carries an AUMID and list all of its top-level
// windows, cloaked or not. This answers how a packaged app's icon maps to a window
// when the visible frame belongs to ApplicationFrameHost.exe.
// For a packaged app the visible window is owned by ApplicationFrameHost.exe, so a
// PID match never finds it. This walks the frame window's descendants looking for a
// child that belongs to a different process - that child is the real app.
int ModeFrame() {
    EnumerateAll();
    for (const auto& row : g_rows) {
        if (row.cls != L"ApplicationFrameWindow" || !row.visible) continue;
        std::printf("frame=%p pid=%lu cloaked=%d title=%s\n", static_cast<void*>(row.hwnd),
                    row.pid, row.cloaked ? 1 : 0, ToUtf8(row.title.c_str()).c_str());
        struct Ctx {
            DWORD frame_pid;
            DWORD real_pid;
        } ctx{row.pid, 0};
        EnumChildWindows(
            row.hwnd,
            [](HWND child, LPARAM param) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(param);
                DWORD pid = 0;
                GetWindowThreadProcessId(child, &pid);
                wchar_t cls[256]{};
                GetClassNameW(child, cls, 255);
                std::printf("    child=%p pid=%lu cls=%s%s\n", static_cast<void*>(child), pid,
                            ToUtf8(cls).c_str(), pid != c->frame_pid ? "  <== other process" : "");
                if (pid != c->frame_pid) c->real_pid = pid;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        std::printf("  real_pid=%lu\n", static_cast<unsigned long>(ctx.real_pid));
    }
    return 0;
}

int ModeProcs() {
    EnumerateAll();
    DWORD pids[4096]{};
    DWORD needed = 0;
    if (!EnumProcesses(pids, sizeof(pids), &needed)) {
        std::printf("EnumProcesses failed\n");
        return 1;
    }
    const DWORD count = needed / sizeof(DWORD);
    std::printf("processes=%lu\n", static_cast<unsigned long>(count));
    int with_aumid = 0;
    for (DWORD i = 0; i < count; ++i) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i]);
        if (proc == nullptr) continue;
        wchar_t aumid[256]{};
        UINT32 len = 256;
        const LONG hr = GetApplicationUserModelId(proc, &len, aumid);
        wchar_t exe[MAX_PATH]{};
        DWORD exe_len = MAX_PATH;
        QueryFullProcessImageNameW(proc, 0, exe, &exe_len);
        CloseHandle(proc);
        if (hr != ERROR_SUCCESS) continue;
        ++with_aumid;
        std::printf("pid=%lu aumid=%s exe=%s\n", static_cast<unsigned long>(pids[i]),
                    ToUtf8(aumid).c_str(), ToUtf8(exe).c_str());
        for (const auto& row : g_rows) {
            if (row.pid != pids[i]) continue;
            std::printf("    win=%p visible=%d cloaked=%d owned=%d tool=%d cls=%s title=%s\n",
                        static_cast<void*>(row.hwnd), row.visible ? 1 : 0,
                        row.cloaked ? 1 : 0, row.owned ? 1 : 0, row.tool ? 1 : 0,
                        ToUtf8(row.cls.c_str()).c_str(), ToUtf8(row.title.c_str()).c_str());
        }
    }
    std::printf("processes_with_aumid=%d\n", with_aumid);
    return 0;
}

int ModePackaged() {
    EnumerateAll();
    std::printf("--- ApplicationFrameHost windows ---\n");
    int found = 0;
    for (const auto& row : g_rows) {
        if (row.cls != L"ApplicationFrameWindow") continue;
        ++found;
        PrintRow(row);
        // The child window inside the frame host belongs to the real app process.
        struct Ctx {
            int depth;
        };
        EnumChildWindows(
            row.hwnd,
            [](HWND child, LPARAM param) -> BOOL {
                DWORD pid = 0;
                GetWindowThreadProcessId(child, &pid);
                wchar_t cls[256]{};
                GetClassNameW(child, cls, 255);
                std::printf("    child=%p pid=%lu cls=%s\n", static_cast<void*>(child), pid,
                            ToUtf8(cls).c_str());
                (void)param;
                return TRUE;
            },
            0);
    }
    std::printf("frame_host_windows=%d\n", found);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring mode = L"enum";
    std::wstring target = L"Notepad";
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--mode=", 0) == 0) mode = arg.substr(7);
        else if (arg.rfind(L"--target=", 0) == 0) target = arg.substr(9);
    }

    if (mode == L"enum") return ModeEnum();
    if (mode == L"activate") return ModeActivate(target);
    if (mode == L"minimize") return ModeMinimize(target);
    if (mode == L"restore") return ModeRestore(target);
    if (mode == L"packaged") return ModePackaged();
    if (mode == L"aumid") return ModeAumid();
    if (mode == L"procs") return ModeProcs();
    if (mode == L"frame") return ModeFrame();
    std::printf("unknown mode\n");
    return 1;
}
