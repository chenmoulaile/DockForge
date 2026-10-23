// DockForge - start an AppEntry.
// Contract: CONTRACTS.md C-08.
//
// Three launch identities exist because the shell itself has three:
//   * a filesystem path          -> CreateProcessW
//   * a packaged app AUMID       -> IApplicationActivationManager
//   * a shell-namespace name     -> ShellExecuteEx on "shell:AppsFolder\..."
//
// Measured on this machine (evidence/T-006/run_launch.txt, run_invoke.txt):
//   ShellExecuteEx("{GUID}\cmd.exe")                -> lasterr=2 (ERROR_FILE_NOT_FOUND)
//   ShellExecuteEx("shell:AppsFolder\{GUID}\cmd.exe") -> ok, process created
//   ActivateApplication("Anysphere.Cursor")         -> 0x80070057 E_INVALIDARG
//   ShellExecuteEx("shell:AppsFolder\Anysphere.Cursor") -> ok, app started
//   IContextMenu::InvokeCommand by menu offset      -> ok (fallback route)
#pragma once

#include <cstdint>

#include "shell/app_entry.h"

namespace dockforge::shell {

enum class LaunchError {
    None = 0,
    NotLaunchable,   // entry has neither a path nor an id
    NotFound,        // path or id no longer resolves
    ActivationFailed,
    ProcessFailed,
};

const char* ToString(LaunchError error);

struct LaunchResult {
    LaunchError error = LaunchError::NotLaunchable;
    std::uint32_t process_id = 0;  // 0 when the shell did not report one
    const char* route = "none";    // which identity was used; for logging and tests

    bool ok() const { return error == LaunchError::None; }
};

// Starts the entry. Never blocks on the started process: a Dock click must return
// immediately so the UI keeps its frame budget.
LaunchResult Launch(const AppEntry& entry);

}  // namespace dockforge::shell
