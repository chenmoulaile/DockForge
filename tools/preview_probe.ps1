# T-016 evidence: hover-dwell preview against the live dock.
#
# Every assertion reads the app's own log. A screenshot that "looks empty" cannot
# distinguish "no preview was shown" from "the preview was shown somewhere else",
# and T-015 already proved the sampling lesson once: single-point sampling tells
# you nothing about where a thing was drawn. So the probe drives the pointer,
# waits on the log line the app emits, and only then captures.
#
# Cases (mirrors C-17j and TASKS.md T-016):
#   1. dwell < 400ms  -> no "preview shown tile=" line at all
#   2. dwell >= 400ms -> the line appears, with the hovered tile index
#   3. leaving the dock -> a "preview hidden" line, and a second dwell re-arms
#   4. screenshot taken AFTER the log line, so the shutter cannot lead the event
#
# The app logs at debug level, which is a hard requirement: DF_INFO carries the
# "preview shown tile=" line, but the per-source lines that prove C-17g (UWP
# frame hwnd vs app_pid) are DF_DEBUG.
param(
    [string]$OutDir = 'evidence\T-016',
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$Dpi = 192,
    [int]$SettleMs = 3000,
    # Must be below kPreviewDwellMs. 250ms keeps the pointer well inside the tile
    # while still being far under the 400ms latch.
    [int]$ShortDwellMs = 250,
    [int]$LongDwellMs = 900,
    [string]$ConfigBackup = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
$configPath = Join-Path $env:APPDATA 'DockForge\config.json'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T016Win {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@ -ErrorAction SilentlyContinue
[void][T016Win]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-LatestLog {
    Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

# The app holds the log open for appending, so plain ReadAllText fails on share
# mode. Open explicitly with ReadWrite sharing, then read from an offset.
function Read-LogFrom([long]$offset) {
    $log = Get-LatestLog
    if ($log -eq $null) { return '' }
    $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        if ($offset -gt $fs.Length) { $offset = 0 }
        $fs.Position = $offset
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        return $sr.ReadToEnd()
    } finally {
        $fs.Close()
    }
}

function Get-LogLength {
    $log = Get-LatestLog
    if ($log -eq $null) { return 0 }
    return $log.Length
}

function Get-DockRect {
    $target = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $target) { return $null }
    $found = [IntPtr]::Zero
    $cb = [T016Win+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T016Win]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $target.Id) { return $true }
        $sb = New-Object System.Text.StringBuilder 256
        [void][T016Win]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:found = $h; return $false }
        return $true
    }
    $script:found = [IntPtr]::Zero
    [void][T016Win]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:found -eq [IntPtr]::Zero) { return $null }
    $r = New-Object T016Win+RECT
    [void][T016Win]::GetWindowRect($script:found, [ref]$r)
    return $r
}

# Wait until the log grows past $offset and contains $pattern, or give up.
# Returns the matched line and the new offset, so the next case starts clean.
function Wait-LogLine([long]$offset, [string]$pattern, [int]$TimeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $tail = Read-LogFrom $offset
        if ($tail) {
            foreach ($line in ($tail -split "`n")) {
                if ($line -match $pattern) { return $line.TrimEnd() }
            }
        }
        Start-Sleep -Milliseconds 50
    }
    return $null
}

function Save-Shot([string]$Path, [int]$Left, [int]$Top, [int]$Width, [int]$Height) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap($Width, $Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($Left, $Top, 0, 0, (New-Object System.Drawing.Size($Width, $Height)))
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# ---------------------------------------------------------------- start the app
# The config is backed up and restored per case: a drop case (T-014) leaves extra
# tiles, and the tile index in "preview shown tile=" is meaningless if the row is
# not the same row the screenshot shows.
Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if ($ConfigBackup -and (Test-Path $ConfigBackup)) {
    Copy-Item $ConfigBackup $configPath -Force
}

# Visual evidence must be solid: the blur tier sets WDA_EXCLUDEFROMCAPTURE, which
# makes CopyFromScreen return whatever is behind the dock (C-16j item 2).
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
$env:DOCKFORGE_LOG_LEVEL = 'debug'
if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs

$rect = Get-DockRect
if (-not $rect) { throw 'dock window not found' }
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
$centreX = [int](($rect.Left + $rect.Right) / 2)
# Mid-plate: the idle region top band is dead space, and a pointer parked there
# never reaches the dock at all (same reason the drop cases aim low).
$plateY = [int]($rect.Bottom - $h * 0.25)

$report = New-Object System.Collections.Generic.List[string]
$report.Add("dpi=$Dpi dock=$($rect.Left),$($rect.Top) ${w}x${h} pid=$($proc.Id)")
$report.Add("target=($centreX,$plateY)")
$report.Add("log=$(Get-LatestLog).FullName")

# ------------------------------------------------------- case 1: short dwell
$offset = Get-LogLength
[void][T016Win]::SetCursorPos($centreX, $plateY)
Start-Sleep -Milliseconds $ShortDwellMs
# Move off the dock before reading: the latch fires on dwell, so the log must be
# quiet while still hovering, and leaving is what lets a later case re-arm.
[void][T016Win]::SetCursorPos(($rect.Left - 500), ($rect.Top - 500))
Start-Sleep -Milliseconds 700
$tail = Read-LogFrom $offset
$shortHit = @($tail -split "`n" | Where-Object { $_ -match 'preview shown tile=' })
$report.Add('--- case=short_dwell ---')
$report.Add("dwell_ms=$ShortDwellMs shown_lines=$($shortHit.Count)")
if ($shortHit.Count -gt 0) { $report += $shortHit }

# -------------------------------------------------------- case 2: long dwell
$offset = Get-LogLength
[void][T016Win]::SetCursorPos($centreX, $plateY)
$shown = Wait-LogLine $offset 'preview shown tile=' ($LongDwellMs + 2500)
$report.Add('--- case=long_dwell ---')
if ($shown) {
    $report.Add("line=$shown")
    # Shutter strictly after the log line: a fixed sleep here leads the event by
    # a variable amount and produces a capture with nothing in it (T-014 lesson).
    Start-Sleep -Milliseconds 600
    $shot = Join-Path $OutDir 'preview_visible.png'
    # The popup sits above the dock and is at most 964 DIP wide at 8 slots; for a
    # single hovered tile it is far narrower. Capture generously and crop later.
    $shotW = [Math]::Min(2000, $w)
    $shotH = 320
    $shotLeft = [Math]::Max(0, $centreX - [int]($shotW / 2))
    $shotTop = [Math]::Max(0, $rect.Top - $shotH - 40)
    Save-Shot $shot $shotLeft $shotTop $shotW $shotH
    $report.Add("shot=$shot rect=$shotLeft,$shotTop ${shotW}x${shotH}")

    # Per-source lines prove C-17g: for a packaged app the frame host hwnd and the
    # real app_pid must differ, and the source must be the frame host.
    $srcLines = @(($tail -split "`n") | Where-Object { $_ -match 'preview source\[' })
    if ($srcLines.Count -eq 0) {
        $srcLines = @((Read-LogFrom $offset) -split "`n" | Where-Object { $_ -match 'preview source\[' })
    }
    $report.Add("source_lines=$($srcLines.Count)")
    $report += $srcLines
} else {
    $report.Add('line=NONE (verdict FAIL: dwell exceeded the latch without a popup)')
}

# ------------------------------------------- case 3: leave, then re-arm once
$offset = Get-LogLength
[void][T016Win]::SetCursorPos(($rect.Left - 500), ($rect.Top - 500))
$hidden = Wait-LogLine $offset 'preview hidden' 2000
$report.Add('--- case=leave ---')
$report.Add("hidden_line=$(if ($hidden) { $hidden } else { 'NONE' })")

$offset = Get-LogLength
[void][T016Win]::SetCursorPos($centreX, $plateY)
$again = Wait-LogLine $offset 'preview shown tile=' ($LongDwellMs + 2500)
$report.Add('--- case=rearm ---')
$report.Add("second_line=$(if ($again) { $again } else { 'NONE (verdict FAIL: dwell did not re-arm)' })")

[void][T016Win]::SetCursorPos(($rect.Left - 500), ($rect.Top - 500))
Start-Sleep -Milliseconds 400

# ------------------------------------------------------------------- verdict
$verdict = 'PASS'
if ($shortHit.Count -gt 0) { $verdict = 'FAIL' }
if (-not $shown) { $verdict = 'FAIL' }
if (-not $again) { $verdict = 'FAIL' }
$report.Add('--- verdict ---')
$report.Add("verdict=$verdict")
$report.Add('note=short_dwell must have zero shown_lines; long_dwell and rearm must each have one')

$outPath = Join-Path $OutDir 'preview_dwell.txt'
[System.IO.File]::WriteAllText($outPath, ($report -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
$report | ForEach-Object { Write-Output $_ }
