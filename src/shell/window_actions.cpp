// DockForge - click behaviour implementation (T-011).
// Contract: CONTRACTS.md C-13.
#include "shell/window_actions.h"

#include <algorithm>

#include "core/frame_clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "shell/app_launcher.h"

namespace dockforge::shell {
namespace {

// One ALT press-and-release. This is the only documented way for a background
// process to earn foreground rights; measured working on this machine
// (evidence/T-011/fg_altkey.txt). It is a fallback, not the primary path: inside a
// real mouse click Windows already grants the right.
void TapAlt() {
    INPUT inputs[2]{};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_MENU;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_MENU;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// Returns false when Windows still refuses after the fallback.
//
// Restoring is asynchronous on purpose. ShowWindow(SW_RESTORE) SENDS the request and
// waits for the target app's message loop to process it, so a busy app blocks the
// dock's UI thread for as long as it likes: measured on this machine, a restore took
// 18-24ms typically and 494ms at the tail (evidence/T-011/click_bench.txt) - thirty
// dropped frames for one click. ShowWindowAsync posts the same request and returns
// immediately; the window comes back a frame or two later, which is indistinguishable
// to the user but keeps the frame budget intact.
//
// SetForegroundWindow is kept synchronous: it is a fast cross-process request that
// does not wait for the app, and it is the call that actually needs the click's
// foreground rights.
bool RaiseNow(HWND hwnd, bool was_iconic, bool& used_alt_fallback) {
    if (was_iconic) {
        if (ShowWindowAsync(hwnd, SW_RESTORE) == FALSE) {
            DF_WARN("shell", "ShowWindowAsync(SW_RESTORE) failed: %lu", GetLastError());
        }
    }
    if (SetForegroundWindow(hwnd) != FALSE) return true;

    used_alt_fallback = true;
    TapAlt();
    if (SetForegroundWindow(hwnd) != FALSE) return true;

    // Last resort: raise without foreground rights. The window becomes visible above
    // its siblings even when it cannot take focus, which is better than a click that
    // appears to do nothing.
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    return GetForegroundWindow() == hwnd;
}

}  // namespace

const char* ToString(WindowAction action) {
    switch (action) {
        case WindowAction::Launched:
            return "launched";
        case WindowAction::Minimized:
            return "minimized";
        case WindowAction::Activated:
            return "activated";
        case WindowAction::Failed:
            return "failed";
        case WindowAction::None:
            break;
    }
    return "none";
}

bool ActivateWindow(HWND hwnd, bool& used_alt_fallback) {
    if (hwnd == nullptr || IsWindow(hwnd) == FALSE) return false;
    return RaiseNow(hwnd, IsIconic(hwnd) != FALSE, used_alt_fallback);
}

ActionResult ActivateOrMinimize(const AppEntry& app, const std::vector<AppWindow>& windows) {
    const std::int64_t started = core::QueryPerformanceTicks();
    ActionResult result;

    AppWindow window;
    if (!FindAppWindow(app, windows, window)) {
        const LaunchResult launched = Launch(app);
        result.action = launched.ok() ? WindowAction::Launched : WindowAction::Failed;
        result.process_id = launched.process_id;
        result.route = "launch";
        result.elapsed_ms = core::QpcToMilliseconds(core::QueryPerformanceTicks() - started);
        DF_INFO("shell", "click %s -> %s (pid=%lu route=%s err=%s)", core::WideToUtf8(app.display_name).c_str(),
                ToString(result.action), static_cast<unsigned long>(result.process_id), result.route,
                ToString(launched.error));
        return result;
    }

    result.process_id = window.app_pid;

    // Sampled BEFORE the action: ActivateWindow restores the window, so asking
    // IsIconic afterwards always reports false and every restore would be logged as a
    // plain raise. Measured on this machine: a restore was reported as route=raise.
    const bool was_iconic = IsIconic(window.hwnd) != FALSE;

    if (GetForegroundWindow() == window.hwnd && !was_iconic) {
        // Asynchronous for the same reason as the restore path: ShowWindow(SW_MINIMIZE)
        // sends and waits for the target app, and a busy app stalls the dock.
        if (ShowWindowAsync(window.hwnd, SW_MINIMIZE) == FALSE) {
            DF_WARN("shell", "ShowWindowAsync(SW_MINIMIZE) failed: %lu", GetLastError());
        }
        result.action = WindowAction::Minimized;
        result.route = "minimize";
    } else {
        const bool raised = ActivateWindow(window.hwnd, result.used_alt_fallback);
        result.action = raised ? WindowAction::Activated : WindowAction::Failed;
        if (!raised) {
            result.route = "none";
        } else {
            result.route = was_iconic ? "restore" : "raise";
        }
    }

    result.elapsed_ms = core::QpcToMilliseconds(core::QueryPerformanceTicks() - started);
    DF_INFO("shell", "click %s -> %s (pid=%lu route=%s alt=%d in %.2fms)",
            core::WideToUtf8(app.display_name).c_str(), ToString(result.action),
            static_cast<unsigned long>(result.process_id), result.route,
            result.used_alt_fallback ? 1 : 0, result.elapsed_ms);
    return result;
}

ActionResult ActivateOrMinimize(const AppEntry& app) {
    return ActivateOrMinimize(app, EnumerateAppWindows());
}

ActionResult ActivateOrMinimizeTimed(const AppEntry& app) {
    // Split timing on purpose. The click budget is 16.7ms, and "the click was slow"
    // is not actionable: enumerating 200 windows through DWM and raising a window are
    // different problems with different fixes. Measured on this machine, enumeration
    // dominated until the filter was reordered (evidence/T-011/click_bench.txt).
    const std::int64_t started = core::QueryPerformanceTicks();
    const std::vector<AppWindow> windows = EnumerateAppWindows();
    const double enumerate_ms =
        core::QpcToMilliseconds(core::QueryPerformanceTicks() - started);

    ActionResult result = ActivateOrMinimize(app, windows);
    result.enumerate_ms = enumerate_ms;
    result.elapsed_ms = core::QpcToMilliseconds(core::QueryPerformanceTicks() - started);
    return result;
}

}  // namespace dockforge::shell
