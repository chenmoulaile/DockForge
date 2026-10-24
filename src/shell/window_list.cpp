// DockForge - which windows belong to one dock tile (T-015).
//
// Contract: C-14f/C-13c forbid a second window-matching implementation, and
// C-14h promises per-window indicators "depending on T-015's window list". This
// is that list.
//
// It is deliberately NOT a new matching rule. dock::IdentityOf (C-15a) already
// defines what makes two entries "the same app" - AUMID, else namespace parsing
// name, else path, else display name - and shell::MatchesEntry already applies
// the window half. This function delegates the whole decision to MatchesEntry.
// Reimplementing the comparison here would be exactly the "second matching
// implementation" C-14f forbids, and would silently diverge the first time the
// identity rules change.
//
// There used to be a pre-filter in front of MatchesEntry: skip windows whose
// identity string differs from the tile's identity string. It was there to avoid
// an O(n) file-identity probe per window, and it was wrong - it reintroduced the
// exact bug it was trying to skip work for. Measured on this machine: the 7-Zip
// tile's identity is
//   C:\Users\chenk\scoop\apps\7zip\current\7zFM.exe
// while the running process reports
//   C:\Users\chenk\scoop\apps\7zip\26.00\7zFM.exe
// so string equality rejected every window and MatchesEntry never ran. Three live
// 7-Zip windows produced windows=0 and no popup. SameExecutable's file-identity
// comparison is the ONLY thing that sees through a scoop junction (known_gotchas,
// T-011), so nothing may run in front of it that compares strings.
//
// The cost is bounded: EnumerateAppWindows returns roughly 10-20 app windows on a
// busy desktop (T-011 measured 1 on an idle one, 198 raw windows), and
// SameExecutable only touches the filesystem when the lowercased paths differ, so
// the common case is a string compare.
//
// The source HWND for the thumbnail falls out of the same enumeration:
//   * a plain Win32 app's own top-level window is the source;
//   * a packaged (UWP) app's visible top-level window is owned by
//     ApplicationFrameHost.exe, and window_registry.cpp records the real app_pid
//     by walking that frame's children (C-13b). The frame host HWND is what DWM
//     can thumbnail, and thumbnail-ing the frame host shows the app's real
//     content, so AppWindow.hwnd is the source for packaged apps too.
// Both are the AppWindow.hwnd that EnumerateAppWindows already reports, so this
// adds no new enumeration.
#include "shell/window_list.h"

namespace dockforge::shell {

std::vector<AppWindow> WindowsForEntry(const AppEntry& entry,
                                       const std::vector<AppWindow>& windows) {
    std::vector<AppWindow> matches;
    for (const AppWindow& window : windows) {
        // MatchesEntry is the single decision point (C-14f). It applies the AUMID
        // rule for packaged apps and SameExecutable's file-identity rule for Win32
        // ones, which is the only comparison that survives a scoop junction.
        if (MatchesEntry(entry, window)) {
            matches.push_back(window);
        }
    }
    return matches;
}

}  // namespace dockforge::shell
