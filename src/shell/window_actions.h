// DockForge - what a Dock click does to an app (T-011).
// Contract: CONTRACTS.md C-13.
//
// One click, three outcomes, decided from live window state:
//   no window          -> start the app
//   window in front    -> minimize it
//   window behind      -> raise it
//
// Foreground rights are the whole difficulty. Measured (evidence/T-011/):
//   SetForegroundWindow from a background process   -> returns 0, refused
//   AttachThreadInput then SetForegroundWindow      -> still refused
//   synthetic ALT tap then SetForegroundWindow      -> works
//   SetForegroundWindow inside WM_LBUTTONDOWN       -> works, no tricks needed
// The last row is why this module is called from the click handler and nowhere
// else: a timer or a hotkey would need the ALT fallback every time.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "shell/app_entry.h"
#include "shell/window_registry.h"

namespace dockforge::shell {

enum class WindowAction {
    None = 0,
    Launched,   // app was not running; started it
    Minimized,  // app had the foreground window
    Activated,  // app was running behind something else
    Failed,
};

const char* ToString(WindowAction action);

struct ActionResult {
    WindowAction action = WindowAction::None;
    std::uint32_t process_id = 0;
    const char* route = "none";  // "launch" | "restore" | "raise" | "minimize" | "none"
    // True when the first SetForegroundWindow was refused and the ALT fallback ran.
    // Recorded rather than hidden: if it ever shows up in a normal session, the
    // click path has lost its foreground rights and that is a bug, not a nuance.
    bool used_alt_fallback = false;
    double elapsed_ms = 0.0;
    // Time spent in EnumerateAppWindows. Reported separately because it is the part
    // that grows with how many windows the desktop has, while the action itself is
    // bounded by one SetForegroundWindow call.
    double enumerate_ms = 0.0;
};

// Decides and performs the action. `windows` is passed in so a batch of clicks (or
// a test) can reuse one enumeration instead of paying EnumWindows per call.
ActionResult ActivateOrMinimize(const AppEntry& app, const std::vector<AppWindow>& windows);

// Convenience wrapper that enumerates first.
ActionResult ActivateOrMinimize(const AppEntry& app);

// Same as above, with the enumeration timed separately so the click latency can be
// attributed instead of guessed at.
ActionResult ActivateOrMinimizeTimed(const AppEntry& app);

// Brings `hwnd` to the foreground, using a synthetic ALT tap when Windows refuses.
// Exposed for tests and for the future window-preview panel.
bool ActivateWindow(HWND hwnd, bool& used_alt_fallback);

}  // namespace dockforge::shell
