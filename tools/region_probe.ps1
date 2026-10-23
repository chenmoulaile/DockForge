# Query the live dock window's region and report which points are inside it.
#
# This is the production window, not a spike double: the point is to prove the
# region the app actually installed, not one a test harness built.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = 'DockForge',
    # When hovered the region legitimately grows over the plate, so the band probes
    # are expected to be inside. Judging both states with the idle rule would report
    # a correct region as broken.
    [switch]$Hovered
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T010R {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern int GetWindowRgn(IntPtr h, IntPtr rgn);
    [DllImport("gdi32.dll")] public static extern bool PtInRegion(IntPtr rgn, int x, int y);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateRectRgn(int l, int t, int r, int b);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr o);
    [DllImport("user32.dll")] public static extern int GetWindowRgnBox(IntPtr h, out RECT box);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static void Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
}
"@
[T010R]::Enable()

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if ($proc -eq $null) { throw "process $ProcessName is not running" }
$targetPid = $proc.Id

$script:dockHandle = [IntPtr]::Zero
$cb = [T010R+EnumProc]{
    param($h, $p)
    $owner = 0
    [T010R]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
    if ($owner -eq $targetPid) {
        $sb = New-Object System.Text.StringBuilder 256
        [T010R]::GetClassName($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:dockHandle = $h; return $false }
    }
    return $true
}
[T010R]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:dockHandle -eq [IntPtr]::Zero) { throw 'dock window not found' }

$r = New-Object T010R+RECT
[T010R]::GetWindowRect($script:dockHandle, [ref]$r) | Out-Null
$w = $r.Right - $r.Left
$h = $r.Bottom - $r.Top

$box = New-Object T010R+RECT
$kind = [T010R]::GetWindowRgnBox($script:dockHandle, [ref]$box)

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("window=$($r.Left),$($r.Top) size=${w}x${h}")
$lines.Add("rgn_box=$($box.Left),$($box.Top),$($box.Right),$($box.Bottom) kind=$kind")
$lines.Add("note kind: 0=ERROR 1=NULLREGION 2=SIMPLEREGION 3=COMPLEXREGION (winreg.h)")

if ($kind -eq 0 -or $kind -eq 1) {
    $lines.Add("verdict=NO_REGION")
} else {
    $rgn = [T010R]::CreateRectRgn(0, 0, 0, 0)
    $copied = [T010R]::GetWindowRgn($script:dockHandle, $rgn)
    $lines.Add("GetWindowRgn=$copied")
    # Scale is derived from the window height, NOT from GetDpiForWindow: the app's
    # DPI override changes the content layout without changing the window's real DPI,
    # so asking Windows reports 192 while the content is laid out at 96 and every
    # probe lands in the wrong place.
    # window_height_dip = icon*max(mag,1) + 2*padding = 48*1.8 + 20 = 106.4
    $scale = $h / 106.4
    $lines.Add("scale_from_height=$([Math]::Round($scale, 3))")
    # Window-local points. The plate occupies the bottom 68 DIP; the reserved band
    # above it must be OUTSIDE the region or it swallows clicks.
    $plateTop = [int]($h - 68 * $scale)
    $probes = @(
        @{ n = 'band_above_plate'; x = [int]($w / 2); y = [int]($plateTop / 2) },
        @{ n = 'band_top_edge';    x = [int]($w / 2); y = 2 },
        @{ n = 'plate_centre';     x = [int]($w / 2); y = [int]($h - 30) },
        @{ n = 'plate_left';       x = 6;             y = [int]($h - 30) },
        @{ n = 'plate_right';      x = $w - 6;        y = [int]($h - 30) },
        @{ n = 'corner_tl';        x = 2;             y = $plateTop + 2 },
        # 20px above the plate: outside the idle region, inside the row when the
        # region has grown for magnified icons.
        @{ n = 'overhang';         x = [int]($w / 2); y = $plateTop - 20 },
        @{ n = 'overhang_far';     x = [int]($w / 2); y = 4 }
    )
    $insideBand = 0
    $insidePlate = 0
    foreach ($probe in $probes) {
        $in = [T010R]::PtInRegion($rgn, $probe.x, $probe.y)
        # WindowFromPoint is the other half of the proof: a point outside the region
        # must resolve to whatever is underneath, which is exactly what a click would
        # hit. PtInRegion alone only says what we intended, not what Windows does.
        $pt = New-Object T010R+POINT
        $pt.X = $r.Left + $probe.x
        $pt.Y = $r.Top + $probe.y
        $hit = [T010R]::WindowFromPoint($pt)
        $hitDock = ($hit -eq $script:dockHandle)
        $lines.Add(("probe {0,-16} ({1},{2}) inside={3} window_from_point=dock:{4}" -f $probe.n, $probe.x, $probe.y, $in, $hitDock))
        if ($probe.n -like 'band*' -and $in) { $insideBand++ }
        if ($probe.n -like 'plate*' -and $in) { $insidePlate++ }
    }
    [T010R]::DeleteObject($rgn) | Out-Null
    $lines.Add("inside_band=$insideBand inside_plate=$insidePlate")
    if ($Hovered) {
        # Hovered: the row must reach above the plate, and the plate must stay clickable.
        $lines.Add("verdict=$(if ($insideBand -ge 1 -and $insidePlate -ge 2) { 'REGION_CORRECT' } else { 'REGION_WRONG' })")
    } else {
        $lines.Add("verdict=$(if ($insideBand -eq 0 -and $insidePlate -ge 2) { 'REGION_CORRECT' } else { 'REGION_WRONG' })")
    }
}

$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
