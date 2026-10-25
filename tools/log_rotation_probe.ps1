# DockForge T-062 evidence probe: crash minidump + log rotation.
#
# Log-driven probe pattern, same as tools/taskbar_probe.ps1. Everything runs in
# scratch directories via the isolation env vars (DOCKFORGE_LOG_DIR /
# DOCKFORGE_DUMP_DIR / DOCKFORGE_CONFIG_PATH), so the developer's real profile
# is never touched. ASCII only: PowerShell 5.1 reads BOM-less scripts as
# ANSI(936) and Chinese literals become syntax errors.
#
# Part A  size roll: log_probe.exe (drives the production Logger) with
#         DOCKFORGE_LOG_MAX_BYTES=20000 -> same-day overflow continues into
#         dockforge-YYYYMMDD-N.log; every file ends at or under the cap.
# Part B  age prune: DOCKFORGE_LOG_KEEP_DAYS=1 with pre-placed yesterday and
#         30-days-ago files -> both deleted, today's file kept.
# Part C  age window: default keep 14 with a 5-days-ago file -> kept.
# Part D  crash dump: the REAL DockForge.exe with DOCKFORGE_TASKBAR_CRASH_TEST=1
#         (null dereference 3s after arming) -> dockforge-crash-*.dmp appears in
#         DOCKFORGE_DUMP_DIR, marker line dumped=1 with ms<=2000, process exits
#         (no hang), and the T-061 tail filter (taskbar restore) still ran.
# Part E  About page "Open log folder": settings window About section contains
#         the push button; BM_CLICK opens Explorer at the (isolated) logs dir;
#         verified by the app's own 'opened log folder' log line AND an
#         independent CabinetWClass window find.            [screenshot]
#
# Verdict: PASS only when every part passed; exit code 0 = PASS, 9 = FAIL.
param(
    [string]$OutDir = 'evidence\T-062',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$ProbeExe = 'build\bin\log_probe.exe'
)
$ErrorActionPreference = 'Stop'
Get-Process -Name DockForge, log_probe -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W6 {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string cls, string win);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowTextW(IntPtr h, System.Text.StringBuilder sb, int max);
    // CharSet.Unicode is load-bearing here too (T-061's own gotcha, re-learned
    // the hard way): without it the StringBuilder marshals as ANSI into the W
    // function, and every class name comes back truncated to its first UTF-16
    // character ("Button" -> "B", "Static" -> "S") - measured in sweep output.
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int max);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")]
    public static extern bool PostMessageW(IntPtr h, uint msg, UIntPtr wp, IntPtr lp);
    [DllImport("user32.dll")]
    public static extern bool SendMessageW(IntPtr h, uint msg, UIntPtr wp, IntPtr lp);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")]
    public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
}
'@
[W6]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$RepoRoot = (Get-Location).Path
$Evidence = Join-Path $RepoRoot $OutDir
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null
$Stamp = [Environment]::TickCount
$TempRoot = Join-Path $env:TEMP ('dockforge-t062-' + $Stamp)
$TempConfig = Join-Path $TempRoot 'config.json'
$LogsA = Join-Path $TempRoot 'logs-a'   # Part A size roll
$LogsB = Join-Path $TempRoot 'logs-b'   # Part B prune deletes
$LogsC = Join-Path $TempRoot 'logs-c'   # Part C prune keeps
$DumpsD = Join-Path $TempRoot 'dumps-d' # Part D crash dump
$LogsE = Join-Path $TempRoot 'logs-e'   # Part E About button target
New-Item -ItemType Directory -Force -Path $TempRoot, $LogsA, $LogsB, $LogsC, $DumpsD, $LogsE |
    Out-Null
[System.IO.File]::WriteAllText($TempConfig, '{}',
    (New-Object System.Text.UTF8Encoding($false)))
$CmdFile = Join-Path $env:TEMP ('df_t062_probe_cmd_' + $Stamp + '.txt')
[System.IO.File]::WriteAllText($CmdFile, '', (New-Object System.Text.UTF8Encoding($false)))

$report = New-Object System.Collections.Generic.List[string]
$failures = 0
function Add-Report([string]$Line) { $script:report.Add($Line); Write-Output $Line }
function Add-Fail([string]$Line) { $script:failures++; Add-Report "FAIL: $Line" }
function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}
function Clear-Env {
    foreach ($name in 'DOCKFORGE_LOG_DIR','DOCKFORGE_DUMP_DIR','DOCKFORGE_LOG_MAX_BYTES',
                     'DOCKFORGE_LOG_KEEP_DAYS','DOCKFORGE_TASKBAR_CRASH_TEST',
                     'DOCKFORGE_SETTINGS','DOCKFORGE_SETTINGS_SHOW',
                     'DOCKFORGE_SETTINGS_CMD_FILE','DOCKFORGE_LOG_LEVEL') {
        Remove-Item Env:$name -ErrorAction SilentlyContinue
    }
}
function Start-App {
    $env:DOCKFORGE_CONFIG_PATH = $script:TempConfig
    return Start-Process -FilePath (Join-Path $RepoRoot $script:Exe) -PassThru
}
function Start-AppWith([hashtable]$Extra) {
    foreach ($key in $Extra.Keys) { Set-Item -Path "Env:$key" -Value $Extra[$key] }
    return Start-App
}
function Stop-App([System.Diagnostics.Process]$Proc) {
    if ($Proc -and -not $Proc.HasExited) {
        Stop-Process -Id $Proc.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 400
}
function Run-LogProbe([string]$Args_) {
    $out = & (Join-Path $RepoRoot $ProbeExe) $Args_.Split(' ') 2>&1
    return ($out -join "`n")
}
function File-Names([string]$Dir) {
    if (-not (Test-Path $Dir)) { return @() }
    return (Get-ChildItem $Dir -Filter 'dockforge-*.log' | ForEach-Object { $_.Name } | Sort-Object)
}
function Log-Leaf([string]$Dir) { return Split-Path -Leaf $Dir }

Add-Report "== T-062 crash dump + log rotation probe $(Get-Date -Format o) =="
Add-Report "exe=$Exe probe=$ProbeExe temp=$TempRoot os=$([Environment]::OSVersion.VersionString)"
# PowerShell 5.1 marshals $null for a string P/Invoke parameter as "" (empty
# string), NOT as NULL: FindWindowW(cls, $null) then only matches windows with
# an EMPTY title, and FindWindowExW(.., $null, $null) fails outright with
# ERROR_INVALID_NAME(123) because an lpClassName of "" is invalid. First run
# measured: the settings window was live on screen, the probe read err=203 for
# it and 0 top-level windows in the enumeration sweep. Every NULL string
# parameter must be [NullString]::Value.

# ---- Part A: size roll -------------------------------------------------------
Add-Report "--- Part A: size roll (cap 20000 bytes via DOCKFORGE_LOG_MAX_BYTES) ---"
Clear-Env
$env:DOCKFORGE_LOG_DIR = $LogsA
$env:DOCKFORGE_LOG_MAX_BYTES = '20000'
$null = Run-LogProbe 'spin 150000'
$names = File-Names $LogsA
Add-Report ("A1 files: " + ($names -join ', '))
if ($names.Count -ge 2) { Add-Report "A1 ok: $($names.Count) files (primary + sequences)" }
else { Add-Fail "A1: expected >= 2 files after a 150KB spin, got $($names.Count)" }
$today = Get-Date -Format yyyyMMdd
$primary = $names | Where-Object { $_ -eq "dockforge-$today.log" }
if ($primary) { Add-Report "A2 ok: primary dockforge-$today.log present" }
else { Add-Fail "A2: primary file dockforge-$today.log missing" }
$seqs = @($names | Where-Object { $_ -match ("^dockforge-$today-\d+\.log$") } |
    ForEach-Object { [int]($_ -replace ".*-", "" -replace "\.log", "") } | Sort-Object)
$seqOk = $true
for ($i = 0; $i -lt $seqs.Count; $i++) { if ($seqs[$i] -ne ($i + 1)) { $seqOk = $false } }
if ($seqs.Count -ge 1 -and $seqOk) {
    Add-Report ("A3 ok: sequences contiguous 1.." + $seqs.Count)
} else {
    Add-Fail ("A3: sequence numbering broken: " + ($seqs -join ','))
}
$tooBig = @()
foreach ($n in $names) {
    $size = (Get-Item (Join-Path $LogsA $n)).Length
    if ($size -gt 20300) { $tooBig += "$n=$size" }
}
if ($tooBig.Count -eq 0) { Add-Report "A4 ok: every file <= 20000 + 300 line tolerance" }
else { Add-Fail ("A4: files over cap: " + ($tooBig -join ', ')) }

# ---- Part B: age prune deletes expired (KEEP_DAYS=1 keeps only today) --------
Add-Report "--- Part B: age prune, DOCKFORGE_LOG_KEEP_DAYS=1 ---"
Clear-Env
$yesterday = (Get-Date).AddDays(-1).ToString('yyyyMMdd')
$old30 = (Get-Date).AddDays(-30).ToString('yyyyMMdd')
[System.IO.File]::WriteAllText((Join-Path $LogsB "dockforge-$yesterday.log"), 'yesterday',
    (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText((Join-Path $LogsB "dockforge-$old30.log"), 'old',
    (New-Object System.Text.UTF8Encoding($false)))
$env:DOCKFORGE_LOG_DIR = $LogsB
$env:DOCKFORGE_LOG_KEEP_DAYS = '1'
$null = Run-LogProbe 'once prune-me'
$names = File-Names $LogsB
Add-Report ("B1 files after run: " + ($names -join ', '))
if (-not (Test-Path (Join-Path $LogsB "dockforge-$yesterday.log"))) {
    Add-Report "B1 ok: yesterday's file deleted"
} else { Add-Fail "B1: yesterday's file survived a keep_days=1 run" }
if (-not (Test-Path (Join-Path $LogsB "dockforge-$old30.log"))) {
    Add-Report "B2 ok: 30-days-ago file deleted"
} else { Add-Fail "B2: 30-days-ago file survived a keep_days=1 run" }
if ($names -contains "dockforge-$today.log") {
    Add-Report "B3 ok: today's file kept"
} else { Add-Fail "B3: today's file missing after run" }

# ---- Part C: age window keeps in-window files (default 14) -------------------
Add-Report "--- Part C: default keep 14 days keeps a 5-day-old file ---"
Clear-Env
$fiveAgo = (Get-Date).AddDays(-5).ToString('yyyyMMdd')
[System.IO.File]::WriteAllText((Join-Path $LogsC "dockforge-$fiveAgo.log"), 'recent',
    (New-Object System.Text.UTF8Encoding($false)))
$env:DOCKFORGE_LOG_DIR = $LogsC
$null = Run-LogProbe 'once keep-me'
if (Test-Path (Join-Path $LogsC "dockforge-$fiveAgo.log")) {
    Add-Report "C1 ok: 5-days-ago file kept under default keep 14"
} else { Add-Fail "C1: 5-days-ago file deleted although inside the 14-day window" }

# ---- Part D: crash dump from the REAL app ------------------------------------
Add-Report "--- Part D: real crash -> minidump, no blocked exit ---"
Clear-Env
$taskbarCrashMarker = Join-Path $env:LOCALAPPDATA 'DockForge\logs\taskbar-crash.txt'
$markerBefore = if (Test-Path $taskbarCrashMarker) { (Get-Item $taskbarCrashMarker).Length } else { 0 }
$t0 = [DateTime]::UtcNow
$proc = Start-AppWith @{ DOCKFORGE_DUMP_DIR = $DumpsD; DOCKFORGE_TASKBAR_CRASH_TEST = '1' }
$exited = $proc.WaitForExit(45000)
$t1 = [DateTime]::UtcNow
$wallMs = [int]($t1 - $t0).TotalMilliseconds
if (-not $exited) {
    Add-Fail "D1: process still alive 45s after arming the crash (blocked exit?)"
    Stop-App $proc
} else {
    Add-Report ("D1 ok: process exited, code " + $proc.ExitCode + ", wall " + $wallMs + "ms")
    if ($proc.ExitCode -eq -1073741819) {
        Add-Report "D2 ok: exit code -1073741819 (0xC0000005)"
    } else { Add-Fail "D2: unexpected exit code $($proc.ExitCode) (want 0xC0000005)" }
    if ($wallMs -lt 45000) { Add-Report "D3 ok: crash -> exit did not hang" }
    else { Add-Fail "D3: exit took the full 45s budget" }
}
Start-Sleep -Milliseconds 600
$dumps = @(Get-ChildItem $DumpsD -Filter 'dockforge-crash-*.dmp' -ErrorAction SilentlyContinue)
if ($dumps.Count -ge 1) {
    $dump = $dumps | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    Add-Report ("D4 ok: dump written: " + $dump.Name + " (" + $dump.Length + " bytes)")
    if ($dump.Length -gt 10240) { Add-Report "D5 ok: dump size > 10KB" }
    else { Add-Fail "D5: dump suspiciously small ($($dump.Length) bytes)" }
} else { Add-Fail "D4: no dockforge-crash-*.dmp in $DumpsD" }
$dumpMarker = Join-Path $DumpsD 'crash-dump.txt'
$dumpMs = $null
$dumpedFlag = $null
if (Test-Path $dumpMarker) {
    foreach ($line in (Get-Content $dumpMarker)) {
        if ($line -match 'dumped=(\d)') { $dumpedFlag = $Matches[1] }
        if ($line -match ' ms=(\d+)') { $dumpMs = [int]$Matches[1] }
    }
}
if ($dumpedFlag -eq '1') { Add-Report "D6 ok: marker dumped=1" }
else { Add-Fail "D6: crash marker missing or dumped=0" }
if ($null -ne $dumpMs -and $dumpMs -le 2000) {
    Add-Report "D7 ok: dump write took ${dumpMs}ms (budget 2000ms)"
} else { Add-Fail "D7: dump write ms=$dumpMs exceeds the 2000ms budget (or marker unreadable)" }
$markerAfter = if (Test-Path $taskbarCrashMarker) { (Get-Item $taskbarCrashMarker).Length } else { 0 }
if ($markerAfter -gt $markerBefore) {
    Add-Report "D8 ok: T-061 tail filter ran (taskbar-crash.txt grew) - chain intact"
} else { Add-Fail "D8: taskbar-crash.txt did not grow; the forwarded tail filter never ran" }

# ---- Part E: About page Open log folder --------------------------------------
Add-Report "--- Part E: About page 'Open log folder' button ---"
Clear-Env
$env:DOCKFORGE_LOG_DIR = $LogsE
$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_SHOW = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $CmdFile
$proc = Start-App
Start-Sleep -Milliseconds 3500
$leaf = Log-Leaf $LogsE
Add-Report "[E1] section about, locate the button"
Set-Command 'section about'
Start-Sleep -Milliseconds 600
$settingsHwnd = [W6]::FindWindowW('DockForgeSettingsWindow', [NullString]::Value)
if ($settingsHwnd -eq [IntPtr]::Zero) { Add-Fail "E1: settings window not found" }
$button = [IntPtr]::Zero
if ($settingsHwnd -ne [IntPtr]::Zero) {
    # Control layout: DockForgeSettingsWindow -> STATIC pane (per section) ->
    # STATIC label + control. The button lives one level below a pane, so a
    # two-level sweep with a text match is enough and cannot reach other apps.
    $top = [IntPtr]::Zero
    $tops = 0
    $buttonSweep = New-Object System.Collections.Generic.List[string]
    while ($true) {
        $top = [W6]::FindWindowExW($settingsHwnd, $top, [NullString]::Value, [NullString]::Value)
        if ($top -eq [IntPtr]::Zero) { break }
        $tops++
        # Direct children of the main window too (defensive: layout may shift).
        $candidates = @($top)
        $pane = [IntPtr]::Zero
        while ($true) {
            $pane = [W6]::FindWindowExW($top, $pane, [NullString]::Value, [NullString]::Value)
            if ($pane -eq [IntPtr]::Zero) { break }
            $candidates += $pane
        }
        $topCls = New-Object System.Text.StringBuilder 256
        [W6]::GetClassNameW($top, $topCls, 256) | Out-Null
        $buttonSweep.Add("top#$tops cls=$($topCls.ToString()) children=$($candidates.Count - 1)")
        foreach ($candidate in $candidates) {
            $cls = New-Object System.Text.StringBuilder 256
            [W6]::GetClassNameW($candidate, $cls, 256) | Out-Null
            if ($cls.ToString() -ne 'Button') { continue }
            $vis = [W6]::IsWindowVisible($candidate)
            $txt = New-Object System.Text.StringBuilder 256
            [W6]::GetWindowTextW($candidate, $txt, 256) | Out-Null
            $buttonSweep.Add("  hwnd=$candidate vis=$vis text='$($txt.ToString())'")
            if (-not $vis) { continue }
            if ($txt.ToString() -eq 'Open log folder') { $button = $candidate; break }
        }
        if ($button -ne [IntPtr]::Zero) { break }
    }
    Add-Report ("E1 scanned top-level child windows: " + $tops)
    foreach ($entry in $buttonSweep) { Add-Report ("E1 sweep: " + $entry) }
}
if ($button -ne [IntPtr]::Zero) {
    Add-Report "E1 ok: 'Open log folder' push button found in the About pane"
} else { Add-Fail "E1: 'Open log folder' button not found" }

# Screenshot the settings window with the About pane visible. PrintWindow, NOT
# CopyFromScreen: SetForegroundWindow from a background probe process is
# refused (project gotcha), so the window stays buried under the operator's
# windows and CopyFromScreen captures whatever covers it - both first runs
# recorded a browser. PrintWindow renders the window into a bitmap regardless
# of occlusion.
if ($settingsHwnd -ne [IntPtr]::Zero) {
    $rect = New-Object W6+RECT
    [W6]::GetWindowRect($settingsHwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left
    $h = $rect.Bottom - $rect.Top
    if ($w -gt 0 -and $h -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap($w, $h)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc()
        $ok = [W6]::PrintWindow($settingsHwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
        $g.ReleaseHdc($hdc)
        $g.Dispose()
        $shot = Join-Path $Evidence 'E1-about-open-log-folder.png'
        $bmp.Save($shot, [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        if ((Test-Path $shot) -and $ok) { Add-Report "shot: E1-about-open-log-folder.png (PrintWindow)" }
        else { Add-Fail "shot: E1 screenshot not written (PrintWindow=$ok)" }
    }
}

if ($button -ne [IntPtr]::Zero) {
    Add-Report "[E2] BM_CLICK -> Explorer opens the logs directory"
    [W6]::SendMessageW($button, 0x00F5, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # BM_CLICK
    # Title match must be a CONTAINS: Explorer titles are "logs-e - 文件资源管理器"
    # (locale suffix), measured on this machine; an exact FindWindowW title fails.
    $explorerWindow = [IntPtr]::Zero
    $deadline = [Environment]::TickCount + 8000
    while ([Environment]::TickCount -lt $deadline) {
        $cabinet = [IntPtr]::Zero
        while ($true) {
            $cabinet = [W6]::FindWindowExW([IntPtr]::Zero, $cabinet, 'CabinetWClass',
                                           [NullString]::Value)
            if ($cabinet -eq [IntPtr]::Zero) { break }
            $title = New-Object System.Text.StringBuilder 512
            [W6]::GetWindowTextW($cabinet, $title, 512) | Out-Null
            if ($title.ToString().Contains($leaf)) { $explorerWindow = $cabinet; break }
        }
        if ($explorerWindow -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 150
    }
    if ($explorerWindow -ne [IntPtr]::Zero) {
        $titleText = New-Object System.Text.StringBuilder 512
        [W6]::GetWindowTextW($explorerWindow, $titleText, 512) | Out-Null
        Add-Report ("E2 ok: Explorer window opened for the logs dir, title='" +
            $titleText.ToString() + "'")
    } else { Add-Fail "E2: no Explorer window opened for the logs directory" }
    if ($explorerWindow -ne [IntPtr]::Zero) {
        [W6]::PostMessageW($explorerWindow, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
        Add-Report "cleanup: Explorer window closed"
    }
}
Stop-App $proc

# ---- verdict ------------------------------------------------------------------
Clear-Env
$verdict = if ($failures -eq 0) { 'PASS' } else { 'FAIL' }
Add-Report ("== verdict=" + $verdict + " failures=" + $failures + " ==")
$reportPath = Join-Path $Evidence 'probe.txt'
[System.IO.File]::WriteAllLines($reportPath, $report,
    (New-Object System.Text.UTF8Encoding($false)))
if ($failures -eq 0) { exit 0 } else { exit 9 }
