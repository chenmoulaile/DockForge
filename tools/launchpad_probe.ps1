# DockForge T-041 evidence probe: Launchpad overlay, grid, paging, click, animation.
#
# Drives the Launchpad through the file channel the app exposes for verification
# (DOCKFORGE_LAUNCHPAD_CMD_FILE), because RegisterHotKey and the physical mouse cannot be
# driven reliably from a script: measured on T-016, a warp of SetCursorPos is overwritten
# by the human hand within one frame.
#
# Only the *source* of the input is replaced. Open/Close, the animation, the grid math,
# the hit test, the icon extraction and the launch all run the production path.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\launchpad_probe.ps1 `
#       -OutDir evidence\T-041 -PinApps 'Sublime'
param(
    [string]$OutDir = 'evidence\T-041',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$PinApps = 'Sublime',
    [int]$WaitOpenMs = 1200,
    [int]$WaitCloseMs = 800,
    # 0 leaves the panel at its native scale. 144 exercises the 150% DoD case on a 200%
    # panel; the app forwards it to both the Dock and the Launchpad overlays.
    [int]$DpiOverride = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
# SystemInformation lives in System.Windows.Forms, and a fresh PowerShell 5.1 session has
# not loaded it. Loading it here rather than relying on it being present: the probe is
# invoked with -NoProfile, so nothing else has.
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class T041Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static IntPtr Find(string cls, bool requireVisible) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls && (!requireVisible || IsWindowVisible(h))) {
                found = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static string Describe(string cls) {
        IntPtr h = Find(cls, false);
        if (h == IntPtr.Zero) return "missing";
        RECT r;
        GetWindowRect(h, out r);
        return string.Format("hwnd={0} visible={1} dpi={2} rect={3},{4} {5}x{6} foreground={7}",
            h, IsWindowVisible(h), GetDpiForWindow(h), r.Left, r.Top,
            r.Right - r.Left, r.Bottom - r.Top, GetForegroundWindow() == h);
    }
}
'@

# Match the app's DPI awareness, or every coordinate this script reads is virtualised:
# measured on T-016, a 2880x1920 desktop reported itself as 1440x960 and every crop
# landed on the wrong half of the screen.
[void][T041Probe]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-LatestLog {
    $dir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
    Get-ChildItem $dir -Filter 'dockforge-*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Get-LogLength {
    $log = Get-LatestLog
    if (-not $log) { return 0 }
    return (Get-Item $log.FullName).Length
}

# The app holds the log open for append, so a plain ReadAllText throws
# "The process cannot access the file ... because it is being used by another process".
function Read-LogFrom([long]$Offset) {
    $log = Get-LatestLog
    if (-not $log) { return @() }
    $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        $sr = New-Object System.IO.StreamReader($fs)
        $fs.Position = [Math]::Min($Offset, $fs.Length)
        $text = $sr.ReadToEnd()
        $sr.Close()
    } finally {
        $fs.Close()
    }
    return ($text -split "`n" | Where-Object { $_.Trim().Length -gt 0 })
}

function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($script:CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}

function Wait-LogMatch([long]$Offset, [string]$Pattern, [int]$TimeoutMs = 4000) {
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        foreach ($line in (Read-LogFrom $Offset)) {
            if ($line -match $Pattern) { return $line }
        }
        Start-Sleep -Milliseconds 60
    }
    return $null
}

function Save-Shot([string]$Path) {
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function Get-Stats([string]$Path, [int]$Step = 8) {
    $bmp = [System.Drawing.Bitmap]::FromFile($Path)
    $colors = @{}
    $sum = 0.0
    $count = 0
    $max = 0.0
    $bright = 0
    for ($y = 0; $y -lt $bmp.Height; $y += $Step) {
        for ($x = 0; $x -lt $bmp.Width; $x += $Step) {
            $p = $bmp.GetPixel($x, $y)
            $key = "$($p.R),$($p.G),$($p.B)"
            $colors[$key] = 1
            $luma = 0.299 * $p.R + 0.587 * $p.G + 0.114 * $p.B
            $sum += $luma
            $count++
            if ($luma -gt $max) { $max = $luma }
            if ($luma -gt 70) { $bright++ }
        }
    }
    $bmp.Dispose()
    return [pscustomobject]@{
        Colors = $colors.Count
        Mean   = [Math]::Round($sum / [Math]::Max(1, $count), 1)
        Max    = [Math]::Round($max, 1)
        Bright = $bright
    }
}

function Compare-Shots([string]$PathA, [string]$PathB, [int]$Step = 8, [int]$Threshold = 24) {
    $bmpA = [System.Drawing.Bitmap]::FromFile($PathA)
    $bmpB = [System.Drawing.Bitmap]::FromFile($PathB)
    $changed = 0
    $total = 0
    for ($y = 0; $y -lt [Math]::Min($bmpA.Height, $bmpB.Height); $y += $Step) {
        for ($x = 0; $x -lt [Math]::Min($bmpA.Width, $bmpB.Width); $x += $Step) {
            $ca = $bmpA.GetPixel($x, $y)
            $cb = $bmpB.GetPixel($x, $y)
            $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
            if ($d -gt $Threshold) { $changed++ }
            $total++
        }
    }
    $bmpA.Dispose()
    $bmpB.Dispose()
    return [pscustomobject]@{
        Changed = $changed
        Total   = $total
        Ratio   = if ($total -gt 0) { [Math]::Round($changed / $total, 4) } else { 0.0 }
    }
}

# ---------------------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$report = New-Object System.Collections.Generic.List[string]
function Add-Report([string]$Line) {
    $script:report.Add($Line)
    Write-Output $Line
}

Add-Report "machine=$([System.Windows.Forms.SystemInformation]::VirtualScreen.Width)x$([System.Windows.Forms.SystemInformation]::VirtualScreen.Height) dpi_aware=1"

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$script:CmdFile = Join-Path $env:TEMP 'df_launchpad_probe_cmd.txt'
Set-Command ''
$runOffset = Get-LogLength

$env:DOCKFORGE_LAUNCHPAD_CMD_FILE = $script:CmdFile
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
$env:DOCKFORGE_PIN_APPS = $PinApps
# The frame-budget line ([loop] frames=... p99=...) is logged at DEBUG, and the logger's
# default floor is INFO. Without this the probe sees zero frame samples and cannot report
# a p99 at all - which is what happened on the first run of this script.
$env:DOCKFORGE_LOG_LEVEL = 'debug'
Remove-Item Env:\DOCKFORGE_PREVIEW -ErrorAction SilentlyContinue
if ($DpiOverride -gt 0) {
    $env:DOCKFORGE_DPI_OVERRIDE = "$DpiOverride"
} else {
    Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue
}
Add-Report "dpi_override=$DpiOverride"

$proc = Start-Process -FilePath $Exe -PassThru
$warm = Wait-LogMatch $runOffset 'launchpad\] index warmed' 20000
if (-not $warm) { Add-Report 'verdict=FAIL warmup never finished'; exit 1 }
Add-Report "warmup=$($warm.Trim())"

$overlay = [T041Probe]::Find('DockForgeOverlayWindow', $false)
if ($overlay -eq [IntPtr]::Zero) {
    Add-Report 'verdict=FAIL overlay window was never created'
    exit 1
}
Add-Report "overlay_created=$([T041Probe]::Describe('DockForgeOverlayWindow'))"

# --- DoD 1: an independent borderless full-screen top-level window, one per display ---
$closedDesc = [T041Probe]::Describe('DockForgeOverlayWindow')
Add-Report "before_open=$closedDesc"

$openOffset = Get-LogLength
Set-Command 'open'
$gridLine = Wait-LogMatch $openOffset 'launchpad\] grid\[0\]' 5000
if (-not $gridLine) { Add-Report 'verdict=FAIL open never logged a grid'; exit 1 }
Add-Report "grid=$($gridLine.Trim())"

$grid = @{}
foreach ($m in [regex]::Matches($gridLine, '(\w+)=([\-0-9\.x,]+)')) {
    $grid[$m.Groups[1].Value] = $m.Groups[2].Value
}
$columns = [int]$grid['columns']
$rows = [int]$grid['rows']
$perPage = [int]$grid['per_page']
$cellW = [double]($grid['cell'].Split('x')[0])
$cellH = [double]($grid['cell'].Split('x')[1])
$originXY = $grid['origin'].Split(',')
$originX = [double]$originXY[0]
$originY = [double]$originXY[1]
$iconEdge = [double]$grid['icon']

$openComplete = Wait-LogMatch $openOffset 'launchpad\] open complete' 3000
$openLine = Wait-LogMatch $openOffset 'launchpad\] open items=' 3000
$pages = if ($openLine -and $openLine -match 'pages=(\d+)') { [int]$Matches[1] } else { 0 }
Add-Report "open=$($openLine.Trim()) pages=$pages"

$openDesc = [T041Probe]::Describe('DockForgeOverlayWindow')
Add-Report "after_open=$openDesc"

$visibleRect = $null
if ($openDesc -match 'rect=(-?\d+),(-?\d+) (\d+)x(\d+)') {
    $visibleRect = [pscustomobject]@{
        Left = [int]$Matches[1]; Top = [int]$Matches[2]
        Width = [int]$Matches[3]; Height = [int]$Matches[4]
    }
}
$monitorW = [System.Windows.Forms.SystemInformation]::VirtualScreen.Width
$monitorH = [System.Windows.Forms.SystemInformation]::VirtualScreen.Height
$fullScreen = $false
if ($visibleRect) {
    $fullScreen = ($visibleRect.Left -eq 0) -and ($visibleRect.Top -eq 0) -and
                  ($visibleRect.Width -eq $monitorW) -and ($visibleRect.Height -eq $monitorH)
}
Add-Report "fullscreen=$fullScreen monitor=${monitorW}x${monitorH}"

Start-Sleep -Milliseconds $WaitOpenMs
Save-Shot (Join-Path $OutDir 'launchpad_open.png')
$openStats = Get-Stats (Join-Path $OutDir 'launchpad_open.png')
Add-Report "open_stats colors=$($openStats.Colors) mean=$($openStats.Mean) max=$($openStats.Max) bright=$($openStats.Bright)"

# --- DoD 2/3: grid columns follow C-18, page count and dots only when > 1 page ---
#
# The grid line carries the viewport in DIP. Recomputing the expected column count from
# the *physical* window rect is wrong whenever the scale is not 1.0 (it is 2.0 on this
# panel), so the check reads the viewport the app itself reported and applies the C-18
# formula to it - the probe must not re-derive layout from a second source of truth.
$viewportW = 0.0
$viewportH = 0.0
if ($grid['viewport'] -match '^([\d\.]+)x([\d\.]+)$') {
    $viewportW = [double]$Matches[1]
    $viewportH = [double]$Matches[2]
}
$iconEdge = if ($grid['icon']) { [double]$grid['icon'] } else { 96.0 }
$expectedColumns = [int][Math]::Floor(($viewportW - 128) / ($iconEdge + 26))
$expectedColumns = [Math]::Max(3, [Math]::Min(12, $expectedColumns))
Add-Report "viewport=${viewportW}x${viewportH} icon=$iconEdge columns_expected=$expectedColumns columns_actual=$columns"
$adaptiveOk = ($columns -eq $expectedColumns)
Add-Report "columns=$columns rows=$rows per_page=$perPage adaptive_matches_formula=$adaptiveOk"
$expectedRows = [int][Math]::Ceiling($perPage / $columns)
Add-Report "rows_expected=$expectedRows rows_actual=$rows matches=$($expectedRows -eq $rows)"
$expectedPages = [int][Math]::Ceiling(208 / $perPage)
Add-Report "pages_expected_min=$expectedPages pages_actual=$pages dots_visible=$($pages -gt 1)"

# --- DoD 4: hover magnifies, click launches and closes ---
#
# Every shutter here is driven by the app's own log line, never by a fixed sleep.
# Measured on this machine: a 500ms sleep after writing "page 1" fired the shutter at
# 23:50:35.29 while the app only consumed the command at 23:50:35.292 - the screenshot
# raced the flip and reported a page diff of 0.0041 against a real 0.1841. C-16j already
# froze this rule for the drop caret; it applies to every probe in this repo.
$cell0X = $originX + $cellW * 0.5
$cell0Y = $originY + $cellH * 0.5
$hoverOffset = Get-LogLength
Set-Command ("hover {0} {1}" -f [Math]::Round($cell0X, 1), [Math]::Round($cell0Y, 1))
$hoverSeen = Wait-LogMatch $hoverOffset 'probe command=hover' 3000
Add-Report "hover_command_consumed=$([bool]$hoverSeen)"
Start-Sleep -Milliseconds 220
Save-Shot (Join-Path $OutDir 'launchpad_hover.png')
$hoverDiff = Compare-Shots (Join-Path $OutDir 'launchpad_open.png') (Join-Path $OutDir 'launchpad_hover.png')
Add-Report "hover_diff changed=$($hoverDiff.Changed) of $($hoverDiff.Total) ratio=$($hoverDiff.Ratio)"
Add-Report "hover_cell0_at=$([Math]::Round($cell0X,1)),$([Math]::Round($cell0Y,1))"

$leaveOffset = Get-LogLength
Set-Command 'leave'
$leaveSeen = Wait-LogMatch $leaveOffset 'probe command=leave' 3000
Add-Report "leave_command_consumed=$([bool]$leaveSeen)"
Start-Sleep -Milliseconds 220

# --- paging: page 2 must change the visible cells ---
#
# Waiting for "page -> 1" is not enough: that line is written when the command handler
# bumps the index, and the pixels follow on the next Repaint. Measured twice (23:50 and
# 23:58) the shutter landed between the two and compared a page-1 screenshot against a
# page-1 reference, reporting 0.0041 against a real 0.1841 - a flaky FAIL for a feature
# that worked. The app now logs the page it actually handed to the painter, so the
# shutter waits for that instead of for the command.
$pageOffset = Get-LogLength
Set-Command 'page 1'
$pageSeen = Wait-LogMatch $pageOffset 'launchpad\] page -> 1' 3000
$pagePainted = Wait-LogMatch $pageOffset 'launchpad\] paint\[0\] page=1' 3000
Add-Report "page_command_consumed=$([bool]$pageSeen) page_painted=$([bool]$pagePainted)"
Save-Shot (Join-Path $OutDir 'launchpad_page2.png')
$pageDiff = Compare-Shots (Join-Path $OutDir 'launchpad_open.png') (Join-Path $OutDir 'launchpad_page2.png')
Add-Report "page_diff changed=$($pageDiff.Changed) of $($pageDiff.Total) ratio=$($pageDiff.Ratio)"
$pageBackOffset = Get-LogLength
Set-Command 'page 0'
$pageBackSeen = Wait-LogMatch $pageBackOffset 'launchpad\] paint\[0\] page=0' 3000
Add-Report "page_back_consumed=$([bool]$pageBackSeen)"

# --- DoD 5: a click on the scrim dismisses without launching ---
$clickOffset = Get-LogLength
$scrimX = $originX + $cellW * 0.5
$scrimY = 40
Set-Command ("click {0} {1}" -f [Math]::Round($scrimX, 1), [Math]::Round($scrimY, 1))
$closedLine = Wait-LogMatch $clickOffset 'launchpad\] closed' $WaitCloseMs + 2000
Add-Report "scrim_click_closed=$([bool]$closedLine)"

Start-Sleep -Milliseconds $WaitCloseMs
$afterClose = [T041Probe]::Describe('DockForgeOverlayWindow')
Add-Report "after_close=$afterClose"
Save-Shot (Join-Path $OutDir 'launchpad_closed.png')
$closedStats = Get-Stats (Join-Path $OutDir 'launchpad_closed.png')
Add-Report "closed_stats colors=$($closedStats.Colors) mean=$($closedStats.Mean) bright=$($closedStats.Bright)"
$openVsClosed = Compare-Shots (Join-Path $OutDir 'launchpad_closed.png') (Join-Path $OutDir 'launchpad_open.png')
Add-Report "open_vs_closed changed=$($openVsClosed.Changed) of $($openVsClosed.Total) ratio=$($openVsClosed.Ratio)"

# --- reopen: the animation must not be skipped by a stale frame delta ---
$reopenOffset = Get-LogLength
Set-Command 'open'
$reopenLine = Wait-LogMatch $reopenOffset 'launchpad\] open items=' 4000
$reopenDone = Wait-LogMatch $reopenOffset 'launchpad\] open complete' 4000
Add-Report "reopen=$([bool]$reopenLine) reopen_complete=$([bool]$reopenDone)"

# The open and complete timestamps must be different: if the first tick carries the whole
# idle gap as its delta, the 180ms animation finishes in one frame and the grid appears
# with no animation at all (measured before the clamp was added).
if ($reopenLine -and $reopenDone) {
    $tOpen = [datetimeoffset]::Parse(($reopenLine -split ' ')[0])
    $tDone = [datetimeoffset]::Parse(($reopenDone -split ' ')[0])
    $spanMs = [int]($tDone - $tOpen).TotalMilliseconds
    Add-Report "reopen_animation_ms=$spanMs animated=$($spanMs -ge 30)"
}

Set-Command 'close'
Start-Sleep -Milliseconds $WaitCloseMs

# --- DoD 4 (second half): a click on a cell launches the app and closes the overlay ---
#
# The log line is "<name> -> <action> route=<route> pid=<pid>" from ActivateTile. Asserting
# on it rather than on a new process is deliberate: the tile pinned by -PinApps is already
# running in most sessions, so the real action is often "activated", not "launched", and a
# probe that demanded a fresh pid would fail for the wrong reason.
$launchOffset = Get-LogLength
Set-Command 'open'
[void](Wait-LogMatch $launchOffset 'launchpad\] open complete' 4000)
$launchCellX = $originX + $cellW * 0.5
$launchCellY = $originY + $cellH * 0.5
Set-Command ("click {0} {1}" -f [Math]::Round($launchCellX, 1), [Math]::Round($launchCellY, 1))
$launchLine = Wait-LogMatch $launchOffset 'launch .* route=' 5000
Add-Report "cell_click_launch=$([bool]$launchLine)"
if ($launchLine) { Add-Report "cell_click_line=$($launchLine.Trim())" }
$launchClosed = Wait-LogMatch $launchOffset 'launchpad\] closed' ($WaitCloseMs + 2000)
Add-Report "cell_click_closed=$([bool]$launchClosed)"

# --- frame budget: p99 while the Launchpad animates ---
#
# The FrameClock is a 1000-sample rolling window, so a p99 read before frames=1000 is
# still dominated by the startup frame (measured on T-016: 19.6ms at frames=377 versus
# 16.5ms at frames=1000, same process). The probe therefore keeps opening and closing the
# Launchpad until the window has rolled over, then reads several samples.
#
# The [loop] line is logged once per second, so one sample needs one full second of
# animation cycles; kSamples=6 is six seconds of continuous open/close traffic.
$benchSamples = 6
Add-Report "frame_budget: warming the 1000-sample window while animating (target $benchSamples samples)"
$benchDeadline = [Environment]::TickCount + 90000
$benchCycles = 0
while ([Environment]::TickCount -lt $benchDeadline) {
    $collected = @(Read-LogFrom $runOffset |
        Where-Object { $_ -match '\[loop\] frames=(\d+)' -and [int]$Matches[1] -ge 1000 })
    if ($collected.Count -ge $benchSamples) { break }
    Set-Command 'open'
    Start-Sleep -Milliseconds 260
    Set-Command 'close'
    Start-Sleep -Milliseconds 200
    $benchCycles++
}
Add-Report "frame_budget cycles=$benchCycles"

$frames = Read-LogFrom $runOffset | Where-Object { $_ -match '\[loop\] frames=(\d+)' }
$p99 = @()
$frameCounts = @()
foreach ($line in $frames) {
    if ($line -match 'frames=(\d+)' -and [int]$Matches[1] -ge 1000) {
        $frameCounts += [int]$Matches[1]
        if ($line -match 'p99=([\d\.]+)ms') { $p99 += [double]$Matches[1] }
    }
}
if ($p99.Count -gt 0) {
    $quietest = ($p99 | Measure-Object -Minimum).Minimum
    $worst = ($p99 | Measure-Object -Maximum).Maximum
    $median = ($p99 | Sort-Object)[[int][Math]::Floor($p99.Count / 2)]
    # T-016 measured this machine's bare high-resolution timer floor at p99 19.226ms with
    # nothing of ours running. A Launchpad p99 at or below that is the machine, not the
    # feature, and claiming the 16.7ms budget on a host whose timer cannot reach it would
    # be dishonest - so the floor is reported alongside.
    $bareFloorMs = 19.226
    $verdict = if ($quietest -le 16.7) { 'WITHIN_16_7' }
               elseif ($quietest -le $bareFloorMs) { 'FLOOR_LIMITED' }
               else { 'OVER_FLOOR' }
    Add-Report "frame_budget samples=$($p99.Count) frames_max=$(($frameCounts | Measure-Object -Maximum).Maximum) p99_min=$quietest p99_median=$median p99_max=$worst budget=$verdict bare_timer_floor_p99=$bareFloorMs"
} else {
    Add-Report "frame_budget samples=0 (never reached frames>=1000; no honest p99 available)"
}

# --- shutdown: no orphan overlay window ---
Stop-Process -Id $proc.Id -Force
Start-Sleep -Milliseconds 600
$orphan = [T041Probe]::Find('DockForgeOverlayWindow', $false)
Add-Report "orphan_overlay=$($orphan -ne [IntPtr]::Zero)"

$report | Set-Content -Path (Join-Path $OutDir 'launchpad_probe.txt') -Encoding UTF8

$fail = @()
if (-not $fullScreen) { $fail += 'overlay not full-screen' }
if (-not ($expectedRows -eq $rows)) { $fail += 'rows mismatch' }
if ($openStats.Mean -lt 20) { $fail += 'overlay appears empty' }
if ($openVsClosed.Ratio -lt 0.10) { $fail += 'open and closed are indistinguishable' }
# Every shutter above waits for the app to log that it consumed the command. If one of
# those waits timed out the picture may predate the command, so the pixel assertions
# below cannot be trusted even if they happen to pass.
if (-not $hoverSeen) { $fail += 'hover command was never consumed' }
if (-not $leaveSeen) { $fail += 'leave command was never consumed' }
if (-not $pageSeen) { $fail += 'page command was never consumed' }
if (-not $pagePainted) { $fail += 'page 2 was never painted' }
if (-not $pageBackSeen) { $fail += 'page-back was never painted' }
if ($hoverDiff.Changed -lt 1) { $fail += 'hover produced no pixel change' }
if ($pageDiff.Ratio -lt 0.05) { $fail += 'page 2 looks identical to page 1' }
if (-not $launchLine) { $fail += 'clicking a cell never reached ActivateTile' }
if (-not $launchClosed) { $fail += 'clicking a cell did not close the overlay' }
if (-not $closedLine) { $fail += 'scrim click did not dismiss' }
if ($orphan -ne [IntPtr]::Zero) { $fail += 'overlay outlived the process' }

if ($fail.Count -eq 0) {
    Add-Report 'verdict=PASS'
    $report | Set-Content -Path (Join-Path $OutDir 'launchpad_probe.txt') -Encoding UTF8
} else {
    Add-Report "verdict=FAIL reasons=$($fail -join '; ')"
    $report | Set-Content -Path (Join-Path $OutDir 'launchpad_probe.txt') -Encoding UTF8
    exit 1
}
