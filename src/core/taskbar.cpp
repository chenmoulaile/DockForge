// DockForge - taskbar show/hide with emergency recovery (T-061).
//
// Implementation facts inherited from the spike (spikes/t061_taskbar):
//   * The taskbar window class is "Shell_TrayWnd" on this OS build (26300);
//     secondary-monitor taskbars are "Shell_SecondaryTrayWnd" (0 on this box,
//     handled by enumeration anyway).
//   * ShowWindowAsync settles IsWindowVisible within ~16ms; the synchronous
//     ShowWindow blocks on explorer's message loop (project gotcha: p99
//     494.59ms on a busy target). Only the async path is used here.
//   * Hiding the window does NOT change the work area (SPI_GETWORKAREA measured
//     identical before/while hidden), so the dock keeps its position.
//   * A restarted explorer recreates Shell_TrayWnd visible on its own - the
//     hide is applied once per config transition and never re-asserted.
#include "core/taskbar.h"

#include "core/log.h"

#include <windows.h>

namespace dockforge::core {
namespace {

constexpr char kLogModule[] = "taskbar";

// The spike measured a ~16ms settle on an idle shell; 500ms is 30x headroom and
// still short enough that a dead explorer fails the call quickly.
constexpr int kSettlePolls = 50;
constexpr int kSettleStepMs = 10;

HWND PrimaryTaskbar() {
    return FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr);
}

// Every secondary-monitor taskbar. The primary is handled separately so
// IsVisible()/WindowExists() stay anchored to the one window that matters.
template <typename Fn>
void ForEachSecondaryTaskbar(Fn&& fn) {
    HWND hwnd = nullptr;
    while ((hwnd = FindWindowExW(nullptr, hwnd, L"Shell_SecondaryTrayWnd", nullptr)) !=
           nullptr) {
        fn(hwnd);
    }
}

// ShowWindowAsync posts and returns; visibility settles when explorer pumps the
// message. Returns the settled state, or false when it never got there.
bool SettleTaskbarVisible(HWND primary, bool want_visible) {
    for (int i = 0; i < kSettlePolls; ++i) {
        if (primary == nullptr ||
            (IsWindowVisible(primary) != FALSE) == want_visible) {
            return true;
        }
        Sleep(kSettleStepMs);
    }
    return primary != nullptr && (IsWindowVisible(primary) != FALSE) == want_visible;
}

}  // namespace

bool ApplyTaskbarVisibility(bool hide, TaskbarBackend& backend, std::string* detail) {
    if (!backend.WindowExists()) {
        if (detail != nullptr) {
            *detail = "taskbar window not found (explorer restarting?)";
        }
        DF_INFO(kLogModule, "apply hide=%d -> no-op: window absent", hide ? 1 : 0);
        return true;
    }
    const bool visible = backend.IsVisible();
    const bool already_in_state = (hide && !visible) || (!hide && visible);
    if (already_in_state) {
        if (detail != nullptr) {
            *detail = hide ? "already hidden" : "already visible";
        }
        DF_INFO(kLogModule, "apply hide=%d -> no-op: %s", hide ? 1 : 0,
                hide ? "already hidden" : "already visible");
        return true;
    }
    const bool ok = hide ? backend.Hide() : backend.Show();
    if (detail != nullptr) {
        *detail = hide ? (ok ? "hidden" : "hide FAILED") : (ok ? "restored" : "restore FAILED");
    }
    DF_INFO(kLogModule, "apply hide=%d visible_before=%d -> %s", hide ? 1 : 0,
            visible ? 1 : 0, ok ? "ok" : "failed");
    return ok;
}

bool ShellTraywndTaskbar::WindowExists() {
    return PrimaryTaskbar() != nullptr;
}

bool ShellTraywndTaskbar::IsVisible() {
    const HWND primary = PrimaryTaskbar();
    return primary != nullptr && IsWindowVisible(primary) != FALSE;
}

bool ShellTraywndTaskbar::Hide() {
    const HWND primary = PrimaryTaskbar();
    if (primary == nullptr) return false;
    ShowWindowAsync(primary, SW_HIDE);
    ForEachSecondaryTaskbar([](HWND hwnd) { ShowWindowAsync(hwnd, SW_HIDE); });
    const bool settled = SettleTaskbarVisible(primary, false);
    if (!settled) {
        DF_ERROR(kLogModule, "hide did not settle within %dms", kSettlePolls * kSettleStepMs);
    }
    return settled;
}

bool ShellTraywndTaskbar::Show() {
    const HWND primary = PrimaryTaskbar();
    if (primary == nullptr) return false;
    ShowWindowAsync(primary, SW_SHOW);
    ForEachSecondaryTaskbar([](HWND hwnd) { ShowWindowAsync(hwnd, SW_SHOW); });
    const bool settled = SettleTaskbarVisible(primary, true);
    if (!settled) {
        DF_ERROR(kLogModule, "show did not settle within %dms", kSettlePolls * kSettleStepMs);
    }
    return settled;
}

bool HideTaskbarSystem(std::string* detail) {
    ShellTraywndTaskbar backend;
    return ApplyTaskbarVisibility(true, backend, detail);
}

bool ForceShowTaskbar(TaskbarBackend& backend, std::string* detail) {
    if (!backend.WindowExists()) {
        if (detail != nullptr) {
            *detail = "taskbar window not found (explorer restarting?)";
        }
        DF_INFO(kLogModule, "restore (forced) -> no-op: window absent");
        return true;
    }
    // Forced show: the emergency path does not consult IsVisible first. A
    // lagging visibility read (ShowWindowAsync is asynchronous) must not stop
    // the recovery; SW_SHOW on a visible window is harmless.
    const bool ok = backend.Show();
    if (detail != nullptr) {
        *detail = ok ? "restored (forced show)" : "restore FAILED (no settle)";
    }
    DF_INFO(kLogModule, "restore (forced) -> %s", ok ? "ok" : "failed");
    return ok;
}

bool RestoreTaskbarSystem(std::string* detail) {
    ShellTraywndTaskbar backend;
    return ForceShowTaskbar(backend, detail);
}

bool TaskbarCurrentlyVisible() {
    const HWND primary = PrimaryTaskbar();
    return primary != nullptr && IsWindowVisible(primary) != FALSE;
}

void InstallTaskbarCrashRecovery() {
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS*) -> LONG {
        // Raw Win32 only. This process is one instruction away from death; the
        // heap may be corrupted, the logger may hold a locked mutex, the CRT
        // may be wedged. FindWindowExW/ShowWindowAsync/WriteFile are the safe
        // subset. Best effort on every step - restore first, marker file last.
        const HWND primary = FindWindowExW(nullptr, nullptr, L"Shell_TrayWnd", nullptr);
        bool restored = false;
        if (primary != nullptr) {
            ShowWindowAsync(primary, SW_SHOW);
            HWND secondary = nullptr;
            while ((secondary = FindWindowExW(nullptr, secondary,
                                              L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
                ShowWindowAsync(secondary, SW_SHOW);
            }
            // No settle poll: Sleep here would just delay death. The show is
            // posted, which is the best a crashing process can promise.
            restored = true;
        }
        wchar_t localappdata[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localappdata, ARRAYSIZE(localappdata)) >
            0) {
            wchar_t path[MAX_PATH]{};
            if (_snwprintf(path, ARRAYSIZE(path) - 1,
                           L"%s\\DockForge\\logs\\taskbar-crash.txt", localappdata) > 0) {
                HANDLE handle = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (handle != INVALID_HANDLE_VALUE) {
                    char line[128]{};
                    const int len = _snprintf(line, sizeof(line) - 1,
                                              "crash filter: taskbar restore posted, "
                                              "window_found=%d tick=%llu\n",
                                              primary != nullptr ? 1 : 0,
                                              static_cast<unsigned long long>(GetTickCount64()));
                    DWORD written = 0;
                    if (len > 0) {
                        WriteFile(handle, line, static_cast<DWORD>(len), &written, nullptr);
                    }
                    CloseHandle(handle);
                }
            }
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
    DF_INFO(kLogModule, "crash recovery filter installed");
}

}  // namespace dockforge::core
