// DockForge - Start-menu / AppsFolder enumeration (CONTRACTS.md C-08).
//
// The Start menu itself reads the shell's AppsFolder, which is the only place where
// Win32 shortcuts and packaged apps appear side by side with a usable label. Walking
// the Programs directories alone would miss all 43 UWP apps on this machine, and
// walking only the registry would miss every shortcut.
#pragma once

#include <string>
#include <vector>

#include "shell/app_entry.h"

namespace dockforge::shell {

struct StartMenuScan {
    std::vector<AppEntry> entries;
    int shortcut_count = 0;    // .lnk and plain file paths
    int uwp_count = 0;         // packaged apps ("...!App")
    int desktop_id_count = 0;  // Win32 apps registered with an AUMID, no path
    int namespace_count = 0;   // shell-namespace children ("{GUID}\name")
    int skipped = 0;           // items whose target could not be resolved
};

// Enumerates the shell AppsFolder. Requires COM to be initialized on this thread.
// Results are de-duplicated by (kind, identity) and sorted by display name so the
// Dock's tile order is stable across runs.
StartMenuScan ScanAppsFolder();

// Convenience wrapper over ScanAppsFolder that returns only the entries.
std::vector<AppEntry> ListApps();

// Recursively collects *.lnk under a directory. Used for the "all programs" grid in
// Launchpad and by tests that need a deterministic, filesystem-only input.
std::vector<std::wstring> FindShortcuts(const std::wstring& root, int max_depth = 4);

}  // namespace dockforge::shell
