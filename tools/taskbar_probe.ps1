# DockForge T-061 evidence probe: taskbar show/hide with emergency recovery.
#
# Drives the LIVE desktop through the production chain and decides PASS/FAIL
# from the app's own log lines plus an independent Win32 read of the taskbar
# window state (FindWindowEx Shell_TrayWnd + IsWindowVisible) - the same
# log-driven shutter pattern as the other probes.
#
# Part A  default off: fresh profile, hide_taskbar=false -> taskbar untouched
# Part B  settings switch on: set behavior.hide_taskbar 1 (RecordEdit -> debounce
#         -> SaveConfig -> ConfigChanged -> apply) -> taskbar HIDDEN  [screenshot]
# Part C  emergency hotkey: real Ctrl+Alt+T via keybd_event -> taskbar RESTORED,
#         and hide_taskbar=false persisted to config.json                 [screenshot]
# Part D  exit restore: hide again, then a graceful WM_CLOSE exit -> taskbar back
# Part E  crash restore: startup config hide_taskbar=true +
#         DOCKFORGE_TASKBAR_CRASH_TEST=1 -> null dereference at 3s ->
#         SetUnhandledExceptionFilter restores; taskbar-crash.txt marker [screenshot]
# Part F  explorer restart: hide, kill explorer.exe, restart it -> the taskbar
#         comes back NATURALLY and the app must not re-hide it (spike Q6,
#         product level)
#
# Cleanup always ends with the taskbar VISIBLE, on every path (manual restore
# issued if a step failed).
param(
    [string]$OutDir = 'evidence\T-061',
    [string]$Exe = 'build\bin\DockForge.exe',
    [switch]$SkipExplorerRestart
)
$ErrorActionPreference = 'Stop'
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Tb {
    // CharSet.Unicode is load-bearing: without it the strings marshal as ANSI
    // into the W function, FindWindowExW never finds "Shell_TrayWnd", and every
    // visibility check lies in BOTH directions (first run measured: baseline
    // read hidden on a visible desktop, "hide" checks passed spuriously,
    // "restore" checks failed spuriously).
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string win);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint msg, UIntPtr wp, IntPtr lp);

    public const uint KEYEVENTF_KEYUP = 0x0002;

    public static bool TaskbarVisible() {
        IntPtr tray = FindWindowExW(IntPtr.Zero, IntPtr.Zero, "Shell_TrayWnd", null);
        return tray != IntPtr.Zero && IsWindowVisible(tray);
    }
    public static void SendCtrlAltT() {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);            // Ctrl down
        keybd_event(0x12, 0, 0, UIntPtr.Zero);            // Alt down
        keybd_event(0x54, 0, 0, UIntPtr.Zero);            // T down
        keybd_event(0x54, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        keybd_event(0x12, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
        keybd_event(0x11, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }
    public static IntPtr DockWindow() {
        return FindWindowW("DockForgeHostWindow", null);
    }
}
'@
[Tb]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$RepoRoot = (Get-Location).Path
$Evidence = Join-Path $RepoRoot $OutDir
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null
$TempDir = Join-Path $env:TEMP ('dockforge-t061-' + [Environment]::TickCount)
$TempConfig = Join-Path $TempDir 'config.json'
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
$CmdFile = Join-Path $env:TEMP 'df_settings_probe_cmd.txt'
[System.IO.File]::WriteAllText($CmdFile, '', (New-Object System.Text.UTF8Encoding($false)))

$report = New-Object System.Collections.Generic.List[string]
$failures = 0
function Add-Report([string]$Line) { $script:report.Add($Line); Write-Output $Line }
function Add-Fail([string]$Line) { $script:failures++; Add-Report "FAIL: $Line" }
function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}
function Get-LatestLog {
    $dir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
    Get-ChildItem $dir -Filter 'dockforge-*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
function Wait-LogMatch([long]$Offset, [string]$Pattern, [int]$TimeoutMs = 10000) {
    $log = Get-LatestLog
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        if ($log) {
            $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
            try {
                $sr = New-Object System.IO.StreamReader($fs)
                $fs.Position = [Math]::Min($Offset, $fs.Length)
                $text = $sr.ReadToEnd(); $sr.Close()
                foreach ($line in ($text -split "`n")) {
                    if ($line -match $Pattern) { $fs.Close(); return $line }
                }
            } finally { if ($fs) { $fs.Close() } }
        }
        Start-Sleep -Milliseconds 60
        $log = Get-LatestLog
    }
    return $null
}
function Wait-Taskbar([bool]$WantVisible, [int]$TimeoutMs = 5000) {
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        if ([Tb]::TaskbarVisible() -eq $WantVisible) { return $true }
        Start-Sleep -Milliseconds 80
    }
    return [Tb]::TaskbarVisible() -eq $WantVisible
}
function Start-App([bool]$CrashTest, [string]$ConfigOverride = $null) {
    # env is per-probe-process; Start-Process inherits it.
    $env:DOCKFORGE_CONFIG_PATH = if ($ConfigOverride) { $ConfigOverride } else { $script:TempConfig }
    $env:DOCKFORGE_SETTINGS = '1'
    $env:DOCKFORGE_SETTINGS_CMD_FILE = $script:CmdFile
    $env:DOCKFORGE_LOG_LEVEL = 'debug'
    if ($CrashTest) { $env:DOCKFORGE_TASKBAR_CRASH_TEST = '1' }
    else { Remove-Item Env:DOCKFORGE_TASKBAR_CRASH_TEST -ErrorAction SilentlyContinue }
    Start-Process -FilePath (Join-Path $RepoRoot $script:Exe) -PassThru
}
function Stop-App([System.Diagnostics.Process]$Proc) {
    if ($Proc -and -not $Proc.HasExited) {
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 500
}
function Manual-Restore {
    # Emergency belt for the operator: only runs when a test step already
    # failed, so an automated failure never leaves a machine without a taskbar.
    Add-Report "cleanup: manual restore issued (taskbar show via ShowWindowAsync)"
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Mr {
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr h, int cmd);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string win);
}
'@
    $tray = [Mr]::FindWindowExW([IntPtr]::Zero, [IntPtr]::Zero, 'Shell_TrayWnd', $null)
    if ($tray -ne [IntPtr]::Zero) { [Mr]::ShowWindowAsync($tray, 5) | Out-Null }  # SW_SHOW
}
function Shot([string]$Name) {
    & (Join-Path $RepoRoot 'tools\screenshot.ps1') -Out (Join-Path $Evidence $Name) -FullScreen |
        Out-Null
    if (Test-Path (Join-Path $Evidence $Name)) {
        Add-Report "shot: $Name"
    } else {
        Add-Fail "shot: $Name was not written"
    }
}

Add-Report "== T-061 taskbar probe $(Get-Date -Format o) =="
Add-Report "exe=$Exe tempconfig=$TempConfig os=$([Environment]::OSVersion.VersionString)"

# ---- Part A: default off -----------------------------------------------------
Add-Report "--- Part A: default off (fresh profile) ---"
Add-Report "[A1] baseline: taskbar visible before anything runs"
if ([Tb]::TaskbarVisible()) { Add-Report "A1 ok: taskbar visible (IsWindowVisible=1)" }
else { Add-Fail "A1: taskbar not visible at probe start; cannot run" }

$offset = (Get-Item (Get-LatestLog).FullName).Length
$proc = Start-App $false
Start-Sleep -Milliseconds 3000
Add-Report "[A2] startup with behavior.hide_taskbar=false: taskbar must stay visible"
$line = Wait-LogMatch $offset 'hide_taskbar off \(default\)'
if ($line) { Add-Report "A2 log: $($line.Trim())" }
else { Add-Fail "A2: no 'hide_taskbar off (default)' log line" }
if ([Tb]::TaskbarVisible()) { Add-Report "A2 ok: taskbar visible with default config" }
else { Add-Fail "A2: default-off profile hid the taskbar" }
Shot 'A2-default-off-visible.png'
Stop-App $proc

# ---- Part B: settings switch on (production chain) ---------------------------
Add-Report "--- Part B: settings switch -> taskbar hidden ---"
$offset = (Get-Item (Get-LatestLog).FullName).Length
$proc = Start-App $false
Start-Sleep -Milliseconds 3000
Add-Report "[B1] set behavior.hide_taskbar 1 (RecordEdit -> debounce -> SaveConfig -> publish)"
Set-Command 'set behavior.hide_taskbar 1'
$line = Wait-LogMatch $offset 'hide_taskbar switch -> 1'
if ($line) { Add-Report "B1 log: $($line.Trim())" }
else { Add-Fail "B1: no 'hide_taskbar switch -> 1' log line" }
if (Wait-Taskbar $false) {
    Add-Report "B1 ok: taskbar HIDDEN (IsWindowVisible=0)"
} else {
    Add-Fail "B1: taskbar still visible after the switch"
}
Start-Sleep -Milliseconds 500
Shot 'B1-hidden.png'
Add-Report "[B2] the switch wrote the flag to disk"
$cfgText = if (Test-Path $TempConfig) { [System.IO.File]::ReadAllText($TempConfig) } else { '' }
if ($cfgText -match '"hide_taskbar"\s*:\s*true') {
    Add-Report "B2 ok: config.json holds hide_taskbar=true"
} else {
    Add-Fail "B2: config.json does not carry hide_taskbar=true"
}

# ---- Part C: emergency hotkey Ctrl+Alt+T -------------------------------------
Add-Report "--- Part C: emergency hotkey Ctrl+Alt+T (real injected keys) ---"
Add-Report "[C1] keybd_event Ctrl+Alt+T -> forced restore + persisted off"
[Tb]::SendCtrlAltT()
$line = Wait-LogMatch $offset 'emergency restore \(Ctrl\+Alt\+T\)'
if ($line) { Add-Report "C1 log: $($line.Trim())" }
else { Add-Fail "C1: no 'emergency restore (Ctrl+Alt+T)' log line" }
if (Wait-Taskbar $true) { Add-Report "C1 ok: taskbar RESTORED by the hotkey" }
else { Add-Fail "C1: taskbar not visible after Ctrl+Alt+T" }
Start-Sleep -Milliseconds 600
Shot 'C1-restored-hotkey.png'
$cfgText2 = if (Test-Path $TempConfig) { [System.IO.File]::ReadAllText($TempConfig) } else { '' }
if ($cfgText2 -match '"hide_taskbar"\s*:\s*false') {
    Add-Report "C2 ok: recovery persisted hide_taskbar=false"
} else {
    Add-Fail "C2: recovery did not persist hide_taskbar=false"
}
Stop-App $proc

# ---- Part D: graceful exit restores ------------------------------------------
Add-Report "--- Part D: graceful exit restores the taskbar ---"
$offset = (Get-Item (Get-LatestLog).FullName).Length
$proc = Start-App $false
Start-Sleep -Milliseconds 3000
Set-Command 'set behavior.hide_taskbar 1'
if (Wait-Taskbar $false) { Add-Report "D1 ok: taskbar hidden again (switch path)" }
else { Add-Fail "D1: taskbar not hidden before the exit test" }
Add-Report "[D2] graceful exit: PostMessage WM_CLOSE to DockForgeHostWindow"
$dock = [Tb]::DockWindow()
if ($dock -eq [IntPtr]::Zero) {
    Add-Fail "D2: dock window not found by class"
} else {
    [Tb]::PostMessageW($dock, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
    if (-not $proc.WaitForExit(10000)) { Add-Fail "D2: process did not exit after WM_CLOSE" }
    $line = Wait-LogMatch $offset 'exit restore: ok'
    if ($line) { Add-Report "D2 log: $($line.Trim())" }
    else { Add-Fail "D2: no 'exit restore: ok' log line" }
    if (Wait-Taskbar $true) { Add-Report "D2 ok: taskbar visible after normal exit" }
    else { Add-Fail "D2: taskbar still hidden after normal exit" }
    Shot 'D2-restored-after-exit.png'
}

# ---- Part E: crash path -------------------------------------------------------
Add-Report "--- Part E: crash -> SetUnhandledExceptionFilter restores ---"
$offset = (Get-Item (Get-LatestLog).FullName).Length
$crashMarker = Join-Path $env:LOCALAPPDATA 'DockForge\logs\taskbar-crash.txt'
$crashSizeBefore = if (Test-Path $crashMarker) { (Get-Item $crashMarker).Length } else { 0 }
# hide_taskbar=true written directly so the STARTUP apply path hides (no
# settings window involved in this leg).
$crashConfig = Join-Path $TempDir 'config-crash.json'
[System.IO.File]::WriteAllText($crashConfig,
    '{"behavior": {"hide_taskbar": true}}', (New-Object System.Text.UTF8Encoding($false)))
$proc = Start-App $true $crashConfig
Start-Sleep -Milliseconds 3000
if (Wait-Taskbar $false) { Add-Report "E1 ok: taskbar hidden by the startup apply" }
else { Add-Fail "E1: startup apply did not hide the taskbar" }
Add-Report "[E2] waiting for the armed null dereference (3s countdown)..."
$exited = $proc.WaitForExit(20000)
if (-not $exited) {
    Add-Fail "E2: process still alive 20s after arming the crash"
    Stop-App $proc
} else {
    Add-Report ("E2 ok: process crashed with exit code " + $proc.ExitCode)
    if (Wait-Taskbar $true) {
        Add-Report "E2 ok: taskbar RESTORED after the crash"
    } else {
        Add-Fail "E2: taskbar still hidden after the crash"
    }
    Start-Sleep -Milliseconds 500
    Shot 'E2-restored-after-crash.png'
    $markerGrew = (Test-Path $crashMarker) -and (Get-Item $crashMarker).Length -gt $crashSizeBefore
    if ($markerGrew) {
        Add-Report ("E3 ok: crash marker appended: " +
            ((Get-Content $crashMarker | Select-Object -Last 1).Trim()))
    } else {
        Add-Fail "E3: taskbar-crash.txt marker missing or unchanged"
    }
}

# ---- Part F: explorer restart = natural recovery ------------------------------
if (-not $SkipExplorerRestart) {
    Add-Report "--- Part F: explorer restart restores naturally (product level) ---"
    $offset = (Get-Item (Get-LatestLog).FullName).Length
    $proc = Start-App $false
    Start-Sleep -Milliseconds 3000
    Set-Command 'set behavior.hide_taskbar 1'
    if (Wait-Taskbar $false) { Add-Report "F1 ok: taskbar hidden (switch path)" }
    else { Add-Fail "F1: taskbar not hidden before the explorer restart test" }
    Add-Report "[F2] taskkill /f /im explorer.exe; restart the shell"
    Start-Process -FilePath 'cmd.exe' -ArgumentList '/c taskkill /f /im explorer.exe' `
        -WindowStyle Hidden -Wait
    Start-Sleep -Milliseconds 800
    if ([Tb]::TaskbarVisible()) { Add-Report "F2 note: taskbar window still present after taskkill" }
    Start-Process -FilePath 'explorer.exe'
    if (Wait-Taskbar $true 30000) {
        Add-Report "F2 ok: taskbar naturally VISIBLE after the shell restart"
    } else {
        Add-Fail "F2: taskbar did not come back within 30s of the shell restart"
    }
    Add-Report "[F3] the app must NOT re-hide (hide applied once per transition)"
    Start-Sleep -Milliseconds 2000
    if ([Tb]::TaskbarVisible()) {
        Add-Report "F3 ok: taskbar still visible; app did not re-assert the hide"
    } else {
        Add-Fail "F3: app re-hid the taskbar after the explorer restart"
    }
    # The app's config still says hide=true here; close it gracefully so the
    # exit restore runs and the machine is left clean.
    $dock = [Tb]::DockWindow()
    if ($dock -ne [IntPtr]::Zero) {
        [Tb]::PostMessageW($dock, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
        $null = $proc.WaitForExit(10000)
    } else {
        Stop-App $proc
    }
    if (Wait-Taskbar $true) { Add-Report "F4 ok: exit restore after the restart test" }
    else { Add-Fail "F4: taskbar not visible after the app exit" }
} else {
    Add-Report "--- Part F: SKIPPED by -SkipExplorerRestart ---"
}

# ---- cleanup -------------------------------------------------------------------
Stop-App $proc
if (-not [Tb]::TaskbarVisible()) { Manual-Restore; Start-Sleep -Milliseconds 800 }
if ([Tb]::TaskbarVisible()) { Add-Report "cleanup ok: taskbar visible" }
else { Add-Fail "cleanup: taskbar NOT visible at probe end" }
Remove-Item Env:DOCKFORGE_SETTINGS -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS_CMD_FILE -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_CONFIG_PATH -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_TASKBAR_CRASH_TEST -ErrorAction SilentlyContinue
Remove-Item $TempDir -Recurse -Force -ErrorAction SilentlyContinue

$verdict = if ($failures -eq 0) { 'PASS' } else { "FAIL($failures)" }
Add-Report "== verdict=$verdict =="
[System.IO.File]::WriteAllLines((Join-Path $Evidence 'verify.txt'), $report,
    (New-Object System.Text.UTF8Encoding($false)))
exit $(if ($failures -eq 0) { 0 } else { 1 })
