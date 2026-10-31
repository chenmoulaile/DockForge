# DockForge test-residue cleanup.
#
# Why this file exists: on 2026-10-26 a probe run left the operator's Windows
# taskbar hidden. Two independent causes, both fixed here and in the probes:
#   1. A leftover `set behavior.hide_taskbar 1` sat in an inherited
#      DOCKFORGE_SETTINGS_CMD_FILE from the previous day. Every probe channel
#      dedups against "the last command I saw", and that memory starts empty, so
#      the stale line executed as if freshly written. Fixed in main.cpp by
#      priming the dedup memory with the file's existing content at startup.
#   2. The probe ended with `Stop-Process -Force`, which is TerminateProcess:
#      neither the normal-exit restore nor the SetUnhandledExceptionFilter crash
#      path can run. Whatever the app changed about the shell stays changed.
#
# This script is the belt to that suspenders. Any probe that launches DockForge
# should call Clear-ProbeResidue from its cleanup path, and a human can run it
# standalone after a probe dies badly.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe_cleanup.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe_cleanup.ps1 -WhatIfOnly
param(
    [switch]$WhatIfOnly,
    # Skip the taskbar restore when the caller knows it never touched the shell.
    [switch]$SkipTaskbar
)
$ErrorActionPreference = 'Continue'

# Every image a probe in this repo can leave behind. Sidecars are children with
# an inherited stdin pipe; a forced parent kill leaves them blocked on a read
# that will never return, so they have to be reaped by name too.
$images = @(
    'DockForge',
    'notification_sidecar',
    'notification_sidecar_stub',
    'hw_sidecar',
    'layout_bench',
    'autostart_probe',
    'log_probe',
    'message_badge_probe_app',
    'spike'
)

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class DfCleanup {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string win);
    [DllImport("user32.dll")]
    public static extern bool ShowWindowAsync(IntPtr h, int cmd);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    public static int RestoreAll() {
        int shown = 0;
        IntPtr primary = FindWindowW("Shell_TrayWnd", null);
        if (primary != IntPtr.Zero) {
            if (!IsWindowVisible(primary)) { ShowWindowAsync(primary, 5); shown++; }
            IntPtr sec = IntPtr.Zero;
            while ((sec = FindWindowExW(IntPtr.Zero, sec, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) {
                if (!IsWindowVisible(sec)) { ShowWindowAsync(sec, 5); shown++; }
            }
        }
        return shown;
    }
}
'@

$killed = @()
foreach ($name in $images) {
    $procs = Get-Process -Name $name -ErrorAction SilentlyContinue
    foreach ($p in $procs) {
        $killed += ("{0}({1})" -f $p.ProcessName, $p.Id)
        if (-not $WhatIfOnly) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

if ($killed.Count -gt 0) {
    $verb = if ($WhatIfOnly) { 'would kill' } else { 'killed' }
    Write-Host ("{0}: {1}" -f $verb, ($killed -join ', '))
} else {
    Write-Host 'no probe processes running'
}

if (-not $SkipTaskbar -and -not $WhatIfOnly) {
    $shown = [DfCleanup]::RestoreAll()
    if ($shown -gt 0) {
        Write-Host ("taskbar: restored {0} hidden window(s)" -f $shown)
    } else {
        Write-Host 'taskbar: already visible'
    }
}

# The scratch configs probes write are the one thing that survives a reboot and
# silently changes the next run (DOCKFORGE_CONFIG_PATH pointing at a stale file
# with hide_taskbar=true is exactly how the 2026-10-26 incident got its config).
$stale = @()
foreach ($var in @('DOCKFORGE_CONFIG_PATH', 'DOCKFORGE_SETTINGS_CMD_FILE',
                   'DOCKFORGE_LOG_DIR', 'DOCKFORGE_TASKBAR_CRASH_TEST')) {
    $val = [Environment]::GetEnvironmentVariable($var)
    if ($val) { $stale += ("{0}={1}" -f $var, $val) }
}
if ($stale.Count -gt 0) {
    Write-Host ("note: this shell still exports {0}" -f ($stale -join '; '))
    Write-Host '      probes inherit these. Start a fresh shell, or unset them.'
}
