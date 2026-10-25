// DockForge - taskbar show/hide with emergency recovery (T-061).
//
// behaviour.hide_taskbar (C-01, default FALSE) hides the Windows taskbar while
// DockForge runs. This is the most invasive thing the app does to the shell, so
// every path out of the hidden state is built first and tested:
//   * the settings switch off (or Ctrl+Alt+T) restores immediately;
//   * a normal exit restores (main wiring);
//   * a crash restores through SetUnhandledExceptionFilter (raw Win32 only);
//   * explorer restarting recreates Shell_TrayWnd visible by itself - the spike
//     measured 0.5s with no help from this process (spikes/t061_taskbar).
#pragma once

#include <string>

namespace dockforge::core {

// The decision layer sits over an injectable backend so the rules below are
// unit-testable without touching the real shell (tests use a fake; the real
// backend is exercised for real by tools/taskbar_probe.ps1):
//
//   hide  + visible  -> Hide   ("hidden")
//   hide  + !visible -> no-op, reported as success ("already hidden")
//   show  + !visible -> Show   ("restored")
//   show  + visible  -> no-op, reported as success ("already visible")
//   window absent    -> success no-op ("taskbar window not found") - explorer
//                       is restarting; the spike proved the replacement window
//                       comes back visible on its own, so there is nothing to
//                       fight here.
//
// Returns false only when an issued action failed to reach its target state;
// `*detail` (when non-null) always gets a one-line human-readable report.
struct TaskbarBackend {
    virtual bool WindowExists() = 0;  // the primary Shell_TrayWnd is present
    virtual bool IsVisible() = 0;     // IsWindowVisible(primary)
    virtual bool Hide() = 0;          // primary + all secondary tray windows
    virtual bool Show() = 0;
    virtual ~TaskbarBackend() = default;
};

bool ApplyTaskbarVisibility(bool hide, TaskbarBackend& backend, std::string* detail);

// The EMERGENCY semantics at the decision layer: always issues Show, never
// consulting IsVisible. A stale or lagging visibility read must not be able to
// talk the recovery out of acting; showing an already visible window is
// harmless. Returns false only when the show failed to settle.
bool ForceShowTaskbar(TaskbarBackend& backend, std::string* detail);

// The real backend. Hides/shows the primary Shell_TrayWnd AND every
// Shell_SecondaryTrayWnd (multi-monitor), using ShowWindowAsync with a bounded
// IsWindowVisible settle poll - the synchronous ShowWindow waits on explorer's
// message loop and the spike measured 16ms async settle, so there is no reason
// to ever block the frame thread on a shell round trip.
class ShellTraywndTaskbar : public TaskbarBackend {
  public:
    bool WindowExists() override;
    bool IsVisible() override;
    bool Hide() override;
    bool Show() override;
};

// One-shot helpers for callers that never hold a backend (main wiring, probes).
bool HideTaskbarSystem(std::string* detail);

// The EMERGENCY one-shot (Ctrl+Alt+T, exit, crash filter): ForceShowTaskbar
// over the real backend.
bool RestoreTaskbarSystem(std::string* detail);

// IsWindowVisible(Shell_TrayWnd), false when the window does not exist.
bool TaskbarCurrentlyVisible();

// Installs the SetUnhandledExceptionFilter that restores the taskbar on any
// unhandled exception. Raw Win32 only inside the filter - the heap, the logger
// and the CRT may all be mid-failure when it runs. Restore attempts from the
// filter append one line to %LOCALAPPDATA%\DockForge\logs\taskbar-crash.txt so
// the probe can prove the path ran without trusting the (possibly dead) logger.
// Installed unconditionally at startup: on a machine where the taskbar was
// never hidden the restore is a no-op show, which costs nothing.
void InstallTaskbarCrashRecovery();

}  // namespace dockforge::core
