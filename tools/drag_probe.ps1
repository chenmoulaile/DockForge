# Drags one Dock tile to a new slot (or off the dock) and reports what the app did.
#
# The drag is injected at the hardware level with mouse_event + SetCursorPos, so it
# exercises the real path: WM_LBUTTONDOWN, SetCapture, a stream of WM_MOUSEMOVE that
# keeps arriving after the pointer leaves the window, and WM_LBUTTONUP. A synthetic
# SendMessage(WM_LBUTTONDOWN) would skip the capture and prove nothing - the whole
# feature depends on capture delivering moves the window would otherwise never see
# (spikes/t013_drag, group A vs group C).
#
# Verification reads the app's own log rather than the pixels: a reorder is a state
# change, and a screenshot cannot distinguish "moved" from "never moved but the icons
# happen to look similar". The log line is also the only record of the source and
# destination indices, which is what the assertions are about.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$FromIndex = 0,
    [int]$ToIndex = 4,
    [int]$TileCount = 12,
    # Drag off the top of the dock: release past the eject margin. Overrides -ToIndex.
    [switch]$Eject,
    # Release immediately on entering the eject zone instead of holding. This is the
    # escape hatch the hold exists for, and it must leave the config untouched.
    [switch]$EjectAbort,
    # How long to hold inside the eject zone, in milliseconds. Must exceed
    # dock::kEjectConfirmMs (550) for a removal to happen.
    [int]$HoldMs = 900,
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$SettleMs = 1200,
    # Number of intermediate pointer positions. Real drags produce dozens; a single
    # jump would cross the threshold in one step and never exercise the ramp.
    [int]$Steps = 14,
    # Capture the screen while the tile is still held, before release. This is the only
    # way to prove the ghost and the insertion line are drawn: after release the ghost
    # is gone, so a screenshot taken afterwards would show a normal dock.
    [string]$ShotPath = '',
    # Capture the same crop with the pointer parked on the release point but NO button
    # down. This is the control for the mid-drag shot: both frames have the same
    # magnification and the same wallpaper, so the difference between them is exactly
    # the ghost, the empty source slot and the insertion line - not "the row grew".
    [string]$HoverShotPath = '',
    # Shifts the release point by this many DIP horizontally. Aiming between two slots
    # separates the ghost from the insertion line: released exactly on a slot centre the
    # two overlap and the screenshot cannot tell them apart.
    [double]$EndOffsetDip = 0.0,
    # Shifts the release point vertically, in DIP. A negative value lifts the ghost into
    # the reserved band above the plate, which is the only place it is drawn clear of
    # the insertion line. Still inside the window, so this is not an eject.
    [double]$EndOffsetYDip = 0.0,
    # Keep the dock running afterwards, so the caller can screenshot the result.
    [switch]$KeepRunning
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class DragNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

[void][DragNative]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))

function Get-DockWindow {
    param([int]$TargetPid)
    $script:dockHwnd = [IntPtr]::Zero
    $cb = [DragNative+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][DragNative]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $TargetPid) { return $true }
        $sb = [System.Text.StringBuilder]::new(256)
        [void][DragNative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') {
            $script:dockHwnd = $h
            return $false
        }
        return $true
    }
    [void][DragNative]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:dockHwnd
}

# Reads only the bytes appended since $Offset. The app holds the log open for append,
# so ReadAllText fails with "being used by another process"; an explicit share mode is
# the only way in.
function Read-NewLog {
    param([string]$Path, [long]$Offset)
    if (-not (Test-Path $Path)) { return @{ Text = ''; Offset = $Offset } }
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$stream.Seek($Offset, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
        $text = $reader.ReadToEnd()
        return @{ Text = $text; Offset = $stream.Position }
    } finally {
        $stream.Dispose()
    }
}

$lines = [System.Collections.Generic.List[string]]::new()
function Add-Line { param([string]$Text) $lines.Add($Text); Write-Host $Text }

$logPath = Join-Path $env:LOCALAPPDATA ('DockForge\logs\dockforge-' + (Get-Date -Format 'yyyyMMdd') + '.log')

Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

# Fresh log offset so the assertions only see this run's gesture.
$logOffset = 0L
if (Test-Path $logPath) { $logOffset = (Get-Item $logPath).Length }

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs
if ($proc.HasExited) { throw "app exited during startup with code $($proc.ExitCode)" }

$dock = Get-DockWindow -TargetPid $proc.Id
if ($dock -eq [IntPtr]::Zero) { throw 'DockForgeHostWindow not found' }

$rect = New-Object DragNative+RECT
[void][DragNative]::GetWindowRect($dock, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Add-Line "dock=($($rect.Left),$($rect.Top)) ${width}x${height} pid=$($proc.Id)"

# Same geometry derivation the other probes use: scale from the window height, because
# the width is sized to the widest possible magnified row and is not a function of the
# DPI alone.
$contentDip = $TileCount * 48 + ($TileCount - 1) * 8
$windowHeightDip = 48 * 1.8 + 20.0
$scale = $height / $windowHeightDip
$windowWidthDip = $width / $scale
$originDip = ($windowWidthDip - $contentDip) / 2.0
$strideDip = 56.0

function Get-SlotPoint {
    param([int]$Index)
    $xDip = $originDip + $Index * $strideDip + 24
    return [pscustomobject]@{
        X = [int][Math]::Round($rect.Left + $xDip * $scale)
        Y = [int][Math]::Round($rect.Bottom - 30 * $scale)
    }
}

$start = Get-SlotPoint -Index $FromIndex
$end = Get-SlotPoint -Index $ToIndex
if ($EndOffsetDip -ne 0.0) {
    $end = [pscustomobject]@{
        X = [int][Math]::Round($end.X + $EndOffsetDip * $scale)
        Y = $end.Y
    }
}
if ($EndOffsetYDip -ne 0.0) {
    $end = [pscustomobject]@{
        X = $end.X
        Y = [int][Math]::Round($end.Y + $EndOffsetYDip * $scale)
    }
}
if ($Eject) {
    # Keep the x so the gesture reads as "lift out", not "drag sideways and happen to
    # leave". The eject zone is measured from the window edge, so this has to clear the
    # 48 DIP margin plus the window top.
    $end = [pscustomobject]@{
        X = $start.X
        Y = [int][Math]::Round($rect.Top - 120 * $scale)
    }
}
Add-Line ("drag scale={0:F3} window={1:F1}dip origin={2:F1}dip from=({3},{4}) to=({5},{6}) eject={7}" -f `
        $scale, $windowWidthDip, $originDip, $start.X, $start.Y, $end.X, $end.Y, $Eject.IsPresent)

# Hover first so the region expands and the press lands on the tile rather than being
# discarded by the trimmed region.
[void][DragNative]::SetCursorPos($start.X, $start.Y)
Start-Sleep -Milliseconds 500

# Control frame: park the pointer on the GRAB point with NO button down, and capture
# the same crop.
#
# It has to be the grab point, not the release point. During a drag the dock keeps
# magnifying around the press position - WM_MOUSEMOVE returns early once the gesture
# starts, so hover.mouse_x_dip never moves. Parking the control at the release point
# would magnify around a different icon and the diff would be dominated by icons
# sliding sideways, which says nothing about the ghost.
if ($HoverShotPath) {
    Add-Type -AssemblyName System.Drawing
    [void][DragNative]::SetCursorPos($start.X, $start.Y)
    Start-Sleep -Milliseconds 500
    $hoverDir = Split-Path -Parent $HoverShotPath
    if ($hoverDir -and -not (Test-Path $hoverDir)) { [void](New-Item -ItemType Directory -Path $hoverDir -Force) }
    $vw = $rect.Right - $rect.Left
    $vh = $rect.Bottom - $rect.Top
    $pad = [int][Math]::Round(120 * $scale)
    $hbmp = New-Object System.Drawing.Bitmap -ArgumentList @([int]$vw, [int]($vh + $pad))
    $hg = [System.Drawing.Graphics]::FromImage($hbmp)
    $hg.CopyFromScreen($rect.Left, ($rect.Top - $pad), 0, 0, $hbmp.Size)
    $hg.Dispose()
    $hbmp.Save($HoverShotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $hbmp.Dispose()
    Add-Line "hover_shot=$HoverShotPath"
}

[DragNative]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)  # LEFTDOWN
Start-Sleep -Milliseconds 80

# GetCapture() is NOT used here on purpose. It returns the capture window of the
# CALLING thread, so from this process it always reports 0 no matter what the app did -
# reading it would have "proven" that capture was never taken. The app logs its own
# GetCapture() result instead, and that line is the assertion.

# Walk to the destination in steps, pausing long enough for the message loop to
# coalesce each position into a frame. A tight loop would deliver every position
# before the tick ever ran, and the drag would appear to teleport - which is exactly
# what the coalescing is designed to do, but it would not prove the intermediate
# frames were drawn.
for ($i = 1; $i -le $Steps; $i++) {
    $t = $i / [double]$Steps
    $x = [int][Math]::Round($start.X + ($end.X - $start.X) * $t)
    $y = [int][Math]::Round($start.Y + ($end.Y - $start.Y) * $t)
    [void][DragNative]::SetCursorPos($x, $y)
    Start-Sleep -Milliseconds 40
}

# Removal is confirmed by a HOLD, so an eject drag has to wait in the zone long enough
# for the confirmation to complete. Releasing immediately is the escape hatch and is
# what -EjectAbort exercises. The wait is 900ms against a 550ms threshold: enough
# margin that a slow frame cannot make the test flaky, and short enough to keep the
# probe fast.
if ($Eject -and -not $EjectAbort) {
    Add-Line "holding in the eject zone for $HoldMs ms"
    Start-Sleep -Milliseconds $HoldMs
}

# Screenshot while the button is still down: the ghost and the insertion line only
# exist during the gesture.
if ($ShotPath) {
    Add-Type -AssemblyName System.Drawing
    $shotDir = Split-Path -Parent $ShotPath
    if ($shotDir -and -not (Test-Path $shotDir)) { [void](New-Item -ItemType Directory -Path $shotDir -Force) }
    $vw = $rect.Right - $rect.Left
    $vh = $rect.Bottom - $rect.Top
    # Include the space above the dock so a lifted ghost is inside the frame.
    $pad = [int][Math]::Round(120 * $scale)
    $bmp = New-Object System.Drawing.Bitmap($vw, ($vh + $pad))
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rect.Left, ($rect.Top - $pad), 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($ShotPath, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Add-Line "shot=$ShotPath"
}

Start-Sleep -Milliseconds 120
[DragNative]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)  # LEFTUP
Start-Sleep -Milliseconds 700

$log = Read-NewLog -Path $logPath -Offset $logOffset
$text = $log.Text

$pressLine = ($text -split "`n" | Where-Object { $_ -match 'drag press tile=(\d+) capture=(\d) match=(\d)' } | Select-Object -Last 1)
$reorderLine = ($text -split "`n" | Where-Object { $_ -match 'reordered tile\[\d+\] -> \[\d+\]' } | Select-Object -Last 1)
$removeLine = ($text -split "`n" | Where-Object { $_ -match 'removed tile\[\d+\]' } | Select-Object -Last 1)
$cancelLine = ($text -split "`n" | Where-Object { $_ -match 'drag cancelled by the system' } | Select-Object -Last 1)

Add-Line "log_press=$($pressLine -ne $null)"
if ($pressLine) { Add-Line "press_line=$($pressLine.Trim())" }
Add-Line "log_reorder=$($reorderLine -ne $null)"
Add-Line "log_remove=$($removeLine -ne $null)"
Add-Line "log_cancel=$($cancelLine -ne $null)"
if ($reorderLine) { Add-Line "reorder_line=$($reorderLine.Trim())" }
if ($removeLine) { Add-Line "remove_line=$($removeLine.Trim())" }
if ($cancelLine) { Add-Line "cancel_line=$($cancelLine.Trim())" }

# The gesture is only correct if the app recorded the slots the drag was aimed at.
# Reporting "reordered" without checking the indices would pass a drag that moved the
# wrong tile.
if ($EjectAbort) {
    # The escape hatch: letting go before the hold completes must leave the dock
    # exactly as it was. Reporting "nothing happened" is the pass condition here, so
    # the verdict is inverted relative to the removal case.
    $verdict = if ($removeLine -or $reorderLine) { 'ABORT_LEAKED' } else { 'ABORTED' }
} elseif ($Eject) {
    $verdict = if ($removeLine) { 'REMOVED' } else { 'NO_REMOVE' }
} elseif ($reorderLine -and $reorderLine -match 'reordered tile\[(\d+)\] -> \[(\d+)\]') {
    $actualFrom = [int]$Matches[1]
    $actualTo = [int]$Matches[2]
    Add-Line "reorder_indices from=$actualFrom to=$actualTo expected_from=$FromIndex expected_to=$ToIndex"
    $verdict = if ($actualFrom -eq $FromIndex -and $actualTo -eq $ToIndex) { 'REORDERED' } else { 'WRONG_INDICES' }
} else {
    $verdict = 'NO_REORDER'
}
Add-Line "verdict=$verdict"

# Persisted state: the order must survive the app, so the config file is the real
# assertion for the DoD line about persistence.
$configPath = Join-Path $env:APPDATA 'DockForge\config.json'
if (Test-Path $configPath) {
    $json = [System.IO.File]::ReadAllText($configPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    $tileList = @()
    if ($json.dock.PSObject.Properties.Name -contains 'tiles' -and $json.dock.tiles) {
        $tileList = @($json.dock.tiles)
    }
    $hiddenList = @()
    if ($json.dock.PSObject.Properties.Name -contains 'hidden' -and $json.dock.hidden) {
        $hiddenList = @($json.dock.hidden)
    }
    Add-Line "config_tiles=$($tileList.Count) config_hidden=$($hiddenList.Count)"
    if ($tileList.Count -gt 0) { Add-Line "config_tile_0=$($tileList[0])" }
    if ($tileList.Count -gt 1) { Add-Line "config_tile_1=$($tileList[1])" }
    if ($hiddenList.Count -gt 0) { Add-Line "config_hidden_0=$($hiddenList[0])" }
} else {
    Add-Line "config_missing=$configPath"
}

Start-Sleep -Milliseconds 300
if (-not $KeepRunning) {
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
}

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
[System.IO.File]::WriteAllText($Out, ($lines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
