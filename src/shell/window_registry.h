// DockForge - which windows exist right now, and which app owns them (T-011).
//
// The dock needs this to answer one question per click: is this app already
// running, and if so which window should be minimized or raised?
//
// Two measured facts shape the whole module (evidence/T-011/):
//
//   * EnumWindows returns ~198 windows on an idle desktop and exactly one of them
//     is an app window. Everything else is cloaked, owned, a tool window, or
//     untitled. Filtering is not optional: minimizing a hidden owner window would
//     look like the click did nothing.
//   * A packaged app's visible window belongs to ApplicationFrameHost.exe, not to
//     the app. Calculator's top-level window has pid 16276 (frame host) while the
//     real app runs in pid 17292, reachable only as a child window of a different
//     process. Matching by top-level pid alone never finds a UWP app.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "shell/app_entry.h"

namespace dockforge::shell {

// One window that a user would recognise as an app window, plus the identity of
// the process that actually owns the app.
struct AppWindow {
    HWND hwnd = nullptr;
    DWORD pid = 0;       // owner of the top-level window (may be the frame host)
    DWORD app_pid = 0;   // process that really runs the app; == pid for Win32
    std::wstring title;
    std::wstring class_name;
    std::wstring exe_path;  // empty when the process could not be queried
    std::wstring aumid;     // empty for unpackaged Win32 apps
};

// Every top-level window that passes the "this is an app window" filter, in the
// z-order EnumWindows returns (topmost first), which is also most-recently-used
// order for practical purposes.
//
// Filter: visible, unowned, not a tool window, not DWM-cloaked, non-empty title.
// `IsWindowVisible` alone is not enough - cloaked windows report visible, which is
// how the shell parks windows on other virtual desktops.
std::vector<AppWindow> EnumerateAppWindows();

// Finds the window that belongs to `app`. Matches by AUMID first (packaged apps and
// AppsFolder entries), then by executable path (Win32 apps). Returns false when the
// app has no window, which the caller reads as "not running".
bool FindAppWindow(const AppEntry& app, const std::vector<AppWindow>& windows,
                   AppWindow& out);

// Case-insensitive comparison after normalizing separators. Exposed for tests:
// path comparison is where a dock silently stops matching after a reinstall moves
// the binary into a versioned folder.
bool SameExecutable(const std::wstring& a, const std::wstring& b);

}  // namespace dockforge::shell
