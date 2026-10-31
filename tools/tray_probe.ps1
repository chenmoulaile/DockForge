# DockForge T-302c evidence probe: the system tray icon and the exit path.
#
# Why this exists. Before T-302c the only way to close DockForge was Task Manager: the
# dock has no title bar, no taskbar button and no window to close. A tray icon whose menu
# has a Quit entry is the fix, and "the icon appeared" is not something a screenshot can
# prove - the notification area is an XAML island on Windows 11 (spike Q2 in
# spikes/T-302_tray/tray_tree.cpp), so there is no ToolbarWindow32 to count buttons in.
# The only honest existence test is Shell_NotifyIconGetRect, which answers for the icon
# itself rather than for a window that may or may not contain it.
#
# Chain under test:
#   A  the icon is registered: Shell_NotifyIconGetRect resolves a non-empty rect, and the
#      rect sits inside the primary display.
#   B  the menu the icon would show carries the entries the DoD requires: a dock toggle
#      that reads "Hide dock" while the dock is visible, a finder bar toggle that is
#      checked, and a Quit entry that is enabled and last.
#   C  the dock toggle really hides the dock and persists: the config file on disk says
#      dock_visible=false, and the dock window is gone.
#   D  the same command brings it back, and the config says true again.
#   E  Quit terminates the process. This is the whole point of the task: no Task Manager.
#   F  the icon is removed before exit, so no ghost icon is left behind.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\tray_probe.ps1
param(
    [string]$OutDir = 'evidence\T-302',
    [string]$Exe = 'build\bin\DockForge.exe'
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$script:failures = New-Object 'System.Collections.Generic.List[string]'

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    $script:failures.Add($why)
}
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  PASS $what" } else { Fail $what }
}
function Stop-All {
    foreach ($n in @('DockForge', 'menu_target')) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

Stop-All
Start-Sleep -Milliseconds 400
if (-not (Test-Path $Exe)) { Write-Host "FAIL: app missing: $Exe"; exit 1 }

$scratch = Join-Path $env:TEMP ('dockforge-t302tray-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch 'config.json'
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
# UTF-8 without BOM: the config reader rejects a BOM and Set-Content -Encoding UTF8
# writes one on PowerShell 5.1.
[System.IO.File]::WriteAllText($configPath, '{}', (New-Object System.Text.UTF8Encoding($false)))

$cmdFile = Join-Path $scratch 'tray-cmd.txt'
$stateFile = Join-Path $scratch 'tray-state.json'
New-Item -ItemType File -Force -Path $cmdFile | Out-Null

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_DUMP_DIR = $scratch
$env:DOCKFORGE_TRAY_CMD_FILE = $cmdFile
$env:DOCKFORGE_TRAY_STATE_FILE = $stateFile
# The finder bar is not what this probe measures, and leaving it on would have it reserve
# the top strip and move the work area - an unrelated system change in the middle of a
# tray test.
$env:DOCKFORGE_FINDERBAR = '0'
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
# The settings window is opt-in; a tray probe that turned it on would be testing two
# features at once. The menu row must grey itself out, and that is what gets asserted.
$settingsAvailable = $false

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T302TrayProbe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    // Every window this process owns whose class starts with "DockForge". Enumerating by
    // class prefix beats FindWindowW: the latter needs an exact name, and it returns 0 for
    // a DPI-virtualized caller, which is a trap this repo has hit before.
    public static string[] Classes() {
        var found = new System.Collections.Generic.List<string>();
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            var name = c.ToString();
            if (name.StartsWith("DockForge")) found.Add(name + (IsWindowVisible(h) ? ":visible" : ":hidden"));
            return true;
        }, IntPtr.Zero);
        return found.ToArray();
    }
    public static IntPtr FindExact(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
[T302TrayProbe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
function Read-Log {
    $file = Latest-Log
    if ($null -eq $file) { return '' }
    # PS 5.1 reads UTF-8 as ANSI by default and Chinese log lines never match without this.
    return (Get-Content $file.FullName -Raw -Encoding UTF8)
}
# The app samples the command file every 50ms and ignores a repeat of the same content,
# so every command carries a unique token.
function Send-Cmd([string]$text, [int]$settleMs = 400) {
    if (Test-Path $stateFile) { Remove-Item $stateFile -Force }
    $body = "$text`n#" + [guid]::NewGuid().ToString('N')
    [System.IO.File]::WriteAllText($cmdFile, $body, (New-Object System.Text.UTF8Encoding($false)))
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path $stateFile) { break }
    }
    Start-Sleep -Milliseconds $settleMs
}
function Get-State {
    if (-not (Test-Path $stateFile)) { return $null }
    try { Get-Content $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $null }
}
function Get-Config {
    try { Get-Content $configPath -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $null }
}
function Menu-Entry($state, [string]$command) {
    if ($null -eq $state) { return $null }
    foreach ($entry in $state.menu) { if ($entry.command -eq $command) { return $entry } }
    return $null
}

Write-Host "=== T-302c tray probe: scratch=$scratch ==="
Start-Process -FilePath (Join-Path $root $Exe) | Out-Null
Start-Sleep -Seconds 4

$app = Get-Process -Name DockForge -ErrorAction SilentlyContinue | Select-Object -First 1
Check ($null -ne $app) "DockForge is running"

# ---- A: the icon exists ---------------------------------------------------
Write-Host '--- A: icon registration ---'
Send-Cmd 'state'
$state = Get-State
Check ($null -ne $state) 'the tray state file was written'
if ($null -ne $state) {
    Check ($state.added -eq $true) 'the tray reports the icon as added'
    $rectOk = $null -ne $state.rect
    Check $rectOk 'Shell_NotifyIconGetRect resolved a rect for the icon'
    if ($rectOk) {
        $r = $state.rect
        $w = $r.right - $r.left
        $h = $r.bottom - $r.top
        Write-Host ("  icon rect = {0},{1},{2},{3} ({4}x{5})" -f $r.left, $r.top, $r.right, $r.bottom, $w, $h)
        Check ($w -gt 0 -and $h -gt 0) 'the icon rect has a real size'
        # The notification area lives on the primary display's taskbar. A rect outside the
        # screen would mean the identifier resolved to something that is not our icon.
        Check ($r.left -ge 0 -and $r.top -ge 0 -and $r.right -le 4096 -and $r.bottom -le 4096) `
            'the icon rect is inside the screen'
    }
}

# ---- B: the menu carries what the DoD asks for ----------------------------
Write-Host '--- B: menu contents ---'
if ($null -ne $state) {
    $toggle = Menu-Entry $state 'toggle_dock'
    $bar = Menu-Entry $state 'toggle_finder_bar'
    $quit = Menu-Entry $state 'quit'
    $settings = Menu-Entry $state 'open_settings'
    Check ($null -ne $toggle) 'the menu has a dock toggle'
    Check ($null -ne $quit) 'the menu has a Quit entry'
    Check ($null -ne $settings) 'the menu has a Settings entry'
    if ($null -ne $toggle) {
        # dock_visible defaults to true, so the row must offer the action, not the state.
        Check ($toggle.label -eq 'Hide Dock') "the dock toggle reads 'Hide Dock' while the dock is visible (got '$($toggle.label)')"
        Check ($toggle.enabled -eq $true) 'the dock toggle is enabled'
    }
    if ($null -ne $quit) {
        Check ($quit.enabled -eq $true) 'Quit is enabled'
        # A default item would fire on Enter or on a stray double click. Quit must never be
        # the one that fires by accident.
        $last = $state.menu[$state.menu.Count - 1]
        Check ($last.command -eq 'quit') 'Quit is the last entry, so it is not the default item'
    }
    if ($null -ne $bar) {
        Check ($bar.checked -eq $true) 'the finder bar toggle is checked (bar enabled in config)'
    }
    if ($null -ne $settings) {
        # The settings window is opt-in at startup (DOCKFORGE_SETTINGS), so this probe
        # runs without it and the row is expected to be greyed out. Asserting the
        # behaviour rather than a fixed value: the row must mirror availability.
        Check ($settings.enabled -eq $settingsAvailable) `
            "Settings availability matches the window (expected $settingsAvailable, got $($settings.enabled))"
    }
    $separators = 0
    foreach ($entry in $state.menu) { if ($entry.separator) { $separators++ } }
    Check ($separators -ge 2) "the menu is grouped by separators (found $separators)"
}

# ---- C: the dock toggle hides the dock and persists ----------------------
Write-Host '--- C: hide the dock ---'
$dockBefore = [T302TrayProbe]::FindExact('DockForgeHostWindow')
Check ($dockBefore -ne [IntPtr]::Zero) 'the dock window exists before hiding'
Send-Cmd 'invoke toggle_dock'
$state = Get-State
Check ($null -ne $state -and $state.last_command -eq 'toggle_dock') 'the probe command reached the dock toggle'
$config = Get-Config
Check ($null -ne $config) 'the config file is readable'
if ($null -ne $config) {
    Check ($config.behavior.dock_visible -eq $false) 'dock_visible was persisted as false'
}
Start-Sleep -Milliseconds 500
# Parenthesised on purpose: `-not X -contains Y` parses as `(-not X) -contains Y`, which
# turns the class list into a boolean and then asks whether that boolean is an array
# element. The assertion silently passed as a type error in the first draft.
Check (-not ([T302TrayProbe]::Classes() -contains 'DockForgeHostWindow:visible')) `
    'the dock window is no longer visible'
$state = Get-State
if ($null -ne $state) {
    $toggle = Menu-Entry $state 'toggle_dock'
    if ($null -ne $toggle) {
        Check ($toggle.label -eq 'Show dock') "the dock toggle now reads 'Show dock' (got '$($toggle.label)')"
    }
}

# ---- D: the same command brings it back ---------------------------------
Write-Host '--- D: show the dock again ---'
Send-Cmd 'invoke toggle_dock'
$config = Get-Config
if ($null -ne $config) {
    Check ($config.behavior.dock_visible -eq $true) 'dock_visible was persisted back to true'
}
Start-Sleep -Milliseconds 600
Check ([T302TrayProbe]::Classes() -contains 'DockForgeHostWindow:visible') 'the dock window is visible again'
# ---- E: Quit terminates the process ------------------------------------
Write-Host '--- E: quit ---'
$logBefore = Read-Log
Send-Cmd 'quit' 200
$exited = $false
for ($i = 0; $i -lt 50; $i++) {
    Start-Sleep -Milliseconds 200
    if ($null -eq (Get-Process -Id $app.Id -ErrorAction SilentlyContinue)) { $exited = $true; break }
}
Check $exited 'the process exited after the Quit command'
$logAfter = Read-Log
Check ($logAfter -match 'tray.*quit requested') 'the log records the Quit command'
# The icon has to be removed before the window dies, or the shell keeps drawing a ghost
# until it next hovers the area.
Check ($logAfter -match 'tray.*removed|tray.*NIM_DELETE') 'the log records the icon removal'

# ---- F: no ghost icon left behind --------------------------------------
Write-Host '--- F: no residue ---'
$leftover = Get-Process -Name DockForge -ErrorAction SilentlyContinue
Check ($null -eq $leftover) 'no DockForge process survived'

Write-Host ''
if ($script:failures.Count -eq 0) {
    Write-Host 'verdict: Q30_tray_icon=PASS Q31_tray_menu=PASS Q32_dock_toggle=PASS Q33_tray_quit=PASS'
    exit 0
}
Write-Host "verdict: FAILED ($($script:failures.Count) checks)"
foreach ($f in $script:failures) { Write-Host "  - $f" }
exit 1
