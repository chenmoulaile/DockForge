// DockForge - window registry implementation (T-011).
// Contract: CONTRACTS.md C-13.
#include "shell/window_registry.h"

#include <appmodel.h>
#include <dwmapi.h>

#include <algorithm>
#include <cwctype>

#include "core/log.h"
#include "core/paths.h"
#include "shell/app_entry.h"

namespace dockforge::shell {
namespace {

struct CollectState {
    std::vector<AppWindow>* out;
};

// A window the user would call "the app's window".
//
// Every clause is load-bearing and was measured against a live desktop
// (evidence/T-011/enum.txt: 198 windows, 1 accepted):
//   visible   - IsWindowVisible is false for most helper windows
//   unowned   - owned windows are dialogs and tooltips; minimizing one is a no-op
//   !tool     - WS_EX_TOOLWINDOW windows never appear in the taskbar
//   !cloaked  - cloaked windows report visible but are on another desktop
//   titled    - untitled windows are message sinks, never user-facing
bool IsAppWindow(HWND hwnd) {
    if (IsWindowVisible(hwnd) == FALSE) return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return false;
    if ((GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) return false;

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0) {
        return false;
    }

    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 511);
    return title[0] != L'\0';
}

std::wstring ExecutablePathOf(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return {};
    wchar_t buffer[MAX_PATH * 2]{};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buffer, &size) != FALSE) {
        result.assign(buffer, size);
    }
    CloseHandle(proc);
    return result;
}

std::wstring AumidOf(DWORD pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return {};
    wchar_t buffer[256]{};
    UINT32 size = static_cast<UINT32>(std::size(buffer));
    std::wstring result;
    if (GetApplicationUserModelId(proc, &size, buffer) == ERROR_SUCCESS) {
        result.assign(buffer);
    }
    CloseHandle(proc);
    return result;
}

struct FrameChildState {
    DWORD frame_pid = 0;
    DWORD real_pid = 0;
};

// The real app behind an ApplicationFrameWindow.
//
// Measured (evidence/T-011/frame.txt): Calculator's frame window is pid 16276
// (ApplicationFrameHost.exe) and one of its children is pid 17292 (CalculatorApp.exe).
// A top-level pid match therefore never finds a packaged app.
BOOL CALLBACK FrameChildProc(HWND child, LPARAM param) {
    auto* state = reinterpret_cast<FrameChildState*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (pid != 0 && pid != state->frame_pid) {
        state->real_pid = pid;
        return FALSE;
    }
    return TRUE;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM param) {
    auto* state = reinterpret_cast<CollectState*>(param);
    if (!IsAppWindow(hwnd)) return TRUE;

    AppWindow row;
    row.hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &row.pid);

    wchar_t buffer[512]{};
    GetWindowTextW(hwnd, buffer, 511);
    row.title = buffer;
    GetClassNameW(hwnd, buffer, 511);
    row.class_name = buffer;

    row.app_pid = row.pid;
    // Only frame-host windows can hide a different process behind them; checking
    // the class first avoids a child enumeration on every ordinary window.
    if (row.class_name == L"ApplicationFrameWindow") {
        FrameChildState child_state{row.pid, 0};
        EnumChildWindows(hwnd, FrameChildProc, reinterpret_cast<LPARAM>(&child_state));
        if (child_state.real_pid != 0) row.app_pid = child_state.real_pid;
    }

    row.exe_path = ExecutablePathOf(row.app_pid);
    row.aumid = AumidOf(row.app_pid);
    state->out->push_back(std::move(row));
    return TRUE;
}

std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return text;
}

// Compares two identities, preferring the strongest available signal.
//
// The order matters more than it looks. A packaged app's visible window is hosted by
// ApplicationFrameHost.exe, so EVERY packaged app shares that executable path. If a
// path match were accepted for an entry that also carries an AUMID, Calculator and
// Settings would resolve to the same window and the dock would raise the wrong app.
// An entry that declares an identity is therefore matched by identity only; the path
// is a fallback for entries that have none (a plain .exe).
bool MatchesEntry(const AppEntry& app, const AppWindow& window) {
    if (!app.aumid.empty()) {
        return !window.aumid.empty() && Lowercase(app.aumid) == Lowercase(window.aumid);
    }
    // Win32 apps registered in AppsFolder (kind ShellItem) carry an AUMID the process
    // does not report and have no path either, so they fall through to a miss here.
    // Their window is found by the launching path they do have, if any.
    if (!app.target_path.empty() && !window.exe_path.empty()) {
        return SameExecutable(app.target_path, window.exe_path);
    }
    return false;
}

// Volume serial + file index for a path, or false when the file cannot be opened.
//
// Comparing paths as strings is not enough, and the failure is silent. Measured on
// this machine: the AppsFolder entry for 7-Zip stores
//   C:\Users\chenk\scoop\apps\7zip\current\7zFM.exe
// because that is what the shell advertises, while the running process reports
//   C:\Users\chenk\scoop\apps\7zip\26.00\7zFM.exe
// The difference is a scoop version junction, so a click launched a second copy
// every time instead of raising the running one. File identity is the only
// comparison that survives junctions, symlinks, 8.3 names and case changes.
bool FileIdentity(const std::wstring& path, BY_HANDLE_FILE_INFORMATION& out) {
    if (path.empty()) return false;
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const BOOL ok = GetFileInformationByHandle(file, &out);
    CloseHandle(file);
    return ok != FALSE;
}

}  // namespace

std::vector<AppWindow> EnumerateAppWindows() {
    std::vector<AppWindow> windows;
    CollectState state{&windows};
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&state));
    return windows;
}

bool FindAppWindow(const AppEntry& app, const std::vector<AppWindow>& windows,
                   AppWindow& out) {
    for (const auto& window : windows) {
        if (!MatchesEntry(app, window)) continue;
        out = window;
        return true;
    }
    return false;
}

bool SameExecutable(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    // Cheap check first: identical strings are the common case and need no I/O.
    if (Lowercase(a) == Lowercase(b)) return true;

    BY_HANDLE_FILE_INFORMATION left{};
    BY_HANDLE_FILE_INFORMATION right{};
    if (!FileIdentity(a, left) || !FileIdentity(b, right)) {
        // One side is not on disk (uninstalled app, unmounted volume). Falling back to
        // the string comparison above is the only answer available, and it already
        // said no.
        return false;
    }
    // Volume serial + file index identifies a file on this machine, which is exactly
    // the scope of a dock match: two files on different volumes can share an index.
    return left.dwVolumeSerialNumber == right.dwVolumeSerialNumber &&
           left.nFileIndexHigh == right.nFileIndexHigh &&
           left.nFileIndexLow == right.nFileIndexLow;
}

}  // namespace dockforge::shell
