# T-010: park the pointer on one icon centre and measure the magnification.
#
# The fisheye peak is `magnification` only when the pointer sits exactly on an icon
# centre; anywhere else the peak is lower by design. So the measurement has to pin
# the cursor to a known icon, settle past the ramp, and then read the icon's height.
#
# Icon i's idle centre in window-local pixels is:
#   padding*scale + icon_size*scale*(i + 0.5) + gap*scale*i
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$ProcessName = 'DockForge',
    [int]$IconIndex = 5,
    [int]$SettleMs = 500
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class T010b {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static void Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
}
"@
[T010b]::Enable()

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if ($proc -eq $null) {
    Start-Process -FilePath $Exe | Out-Null
    Start-Sleep -Milliseconds 1600
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
    if ($proc -eq $null) { throw 'process did not start' }
}
$targetPid = $proc.Id
Start-Sleep -Milliseconds 400

$script:dockHandle = [IntPtr]::Zero
$cb = [T010b+EnumProc]{
    param($h, $p)
    $owner = 0
    [T010b]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
    if ($owner -eq $targetPid) {
        $sb = New-Object System.Text.StringBuilder 256
        [T010b]::GetClassName($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:dockHandle = $h; return $false }
    }
    return $true
}
[T010b]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:dockHandle -eq [IntPtr]::Zero) { throw 'dock window not found' }

$r = New-Object T010b+RECT
[T010b]::GetWindowRect($script:dockHandle, [ref]$r) | Out-Null
# Scale comes from the window's own height, not from GetDpiForWindow: the app's
# DOCKFORGE_DPI_OVERRIDE lays the content out at the override while the window's
# real DPI stays 192, so asking Windows reports the wrong scale for the pixels that
# are actually drawn. window_height_dip is icon*max(mag,1) + 2*padding = 106.4.
$realDpi = [T010b]::GetDpiForWindow($script:dockHandle)
$scale = ($r.Bottom - $r.Top) / 106.4
$dpi = [int][Math]::Round($scale * 96)

function Capture-Rect {
    param([string]$Path, $Rect)
    $w = $Rect.Right - $Rect.Left
    $h = $Rect.Bottom - $Rect.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($Rect.Left, $Rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Idle reference first, with the cursor parked far away.
[T010b]::SetCursorPos(($r.Left - 400), ($r.Top - 400)) | Out-Null
Start-Sleep -Milliseconds 600
$idlePath = "$Out.idle.png"
Capture-Rect -Path $idlePath -Rect $r

# Park on the requested icon's idle centre.
#
# The row is centred in a window that is wider than the plate, because the window
# must also hold the magnified row. So the content origin is NOT the plate padding:
# it is (window_width - content_width)/2. Using the padding here puts the cursor
# 35 DIP off centre, which reads as "the peak scale is 1.58 instead of 1.8" and
# looks like a layout bug when it is only a measurement bug.
$iconSize = 48
$gap = 8
$tileCount = 12
$contentWidthDip = $iconSize * $tileCount + $gap * ($tileCount - 1)
$windowWidthDip = ($r.Right - $r.Left) / $scale
$rowOriginDip = ($windowWidthDip - $contentWidthDip) / 2.0
$centreLocal = [int](($rowOriginDip + $iconSize * ($IconIndex + 0.5) + $gap * $IconIndex) * $scale)
# The plate is 68 DIP tall and sits on the bottom edge, so a point 30 DIP above the
# bottom is inside it at every scale. Using a fixed pixel offset instead would land
# outside the plate at 96 DPI and the hover would never start.
$centreY = [int]($r.Bottom - 30 * $scale)
# The vertical target must be inside the plate, not in the empty band above it,
# otherwise the pointer is not over the dock and hover never starts.

[T010b]::SetCursorPos(($r.Left + $centreLocal), $centreY) | Out-Null
Start-Sleep -Milliseconds $SettleMs
$hoverPath = "$Out.hover.png"
Capture-Rect -Path $hoverPath -Rect $r

function Measure-Overhang {
    param([string]$Path, [int]$PlateTop)
    $bmp = [System.Drawing.Bitmap]::FromFile($Path)
    $minX = 99999; $maxX = -1; $minY = 99999
    for ($x = 0; $x -lt $bmp.Width; $x++) {
        for ($y = 0; $y -lt $PlateTop; $y++) {
            $p = $bmp.GetPixel($x, $y)
            # Anything that is neither the desktop nor fully transparent counts as
            # icon content.
            if (($p.R -gt 6 -or $p.G -gt 6 -or $p.B -gt 6) -and $p.A -gt 8) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
                if ($y -lt $minY) { $minY = $y }
            }
        }
    }
    $bmp.Dispose()
    if ($maxX -lt 0) { return @{ any = $false } }
    return @{ any = $true; minX = $minX; maxX = $maxX; minY = $minY }
}

# The plate top in window-local pixels: window height minus plate height (68 DIP).
$plateTop = [int](($r.Bottom - $r.Top) - 68 * $scale)
$idleM = Measure-Overhang -Path $idlePath -PlateTop $plateTop
$hoverM = Measure-Overhang -Path $hoverPath -PlateTop $plateTop

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("window rect=$($r.Left),$($r.Top) size=$($r.Right - $r.Left)x$($r.Bottom - $r.Top)")
$lines.Add("dpi=$dpi (window reports $realDpi) scale=$([Math]::Round($scale,3)) plate_top_px=$plateTop")
$lines.Add("cursor=$($r.Left + $centreLocal),$centreY (icon $IconIndex, local x=$centreLocal)")
$lines.Add("idle_overhang=$(if ($idleM.any) { "$($idleM.minX)..$($idleM.maxX) top=$($idleM.minY)" } else { 'NONE' })")
$lines.Add("hover_overhang=$(if ($hoverM.any) { "$($hoverM.minX)..$($hoverM.maxX) top=$($hoverM.minY)" } else { 'NONE' })")

if ($hoverM.any) {
    # Baseline is the plate's inner edge: plate bottom minus one padding.
    $baseline = ($r.Bottom - $r.Top) - [int](10 * $scale)
    $heightPx = $baseline - $hoverM.minY
    $heightDip = $heightPx / $scale
    $scaleObserved = $heightDip / 48.0
    $lines.Add("baseline_px=$baseline icon_height_px=$heightPx icon_height_dip=$([Math]::Round($heightDip, 1))")
    $lines.Add("observed_scale=$([Math]::Round($scaleObserved, 3))")
    $lines.Add("verdict=$(if ($scaleObserved -gt 1.05) { 'MAGNIFIED' } else { 'NO_MAGNIFICATION' })")
} else {
    $lines.Add("verdict=NO_OVERHANG")
}

$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
