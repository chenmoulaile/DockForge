# tools/pattern_contrast.ps1 - measure whether a fine pattern behind a translucent
# window is still visible (alpha blend) or destroyed (real blur).
#
# A flat-colour backdrop can only prove "translucent". A checkerboard backdrop plus
# a contrast measurement can distinguish:
#   high contrast  -> the pattern survived: the material is a plain alpha blend
#   near-zero      -> the pattern was blurred away: the material really blurs
#
#   powershell.exe -File tools/pattern_contrast.ps1 -Out report.txt -Rect 200,200,480,200
#
# -Rect is "x,y,w,h" in physical pixels. Reported value is the mean absolute
# difference between horizontally adjacent pixels (0-255 scale), plus min/max
# luminance so a solid fill is not mistaken for a perfectly blurred one.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Rect,
    [string]$Image = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Dpi3 {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    public static void Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
}
'@
[Dpi3]::Enable()

$parts = $Rect.Split(',')
if ($parts.Count -ne 4) { throw "Rect must be 'x,y,w,h', got '$Rect'" }
$rx = [int]$parts[0]; $ry = [int]$parts[1]; $rw = [int]$parts[2]; $rh = [int]$parts[3]

$bmp = New-Object System.Drawing.Bitmap($rw, $rh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rx, $ry, 0, 0, (New-Object System.Drawing.Size($rw, $rh)))
$g.Dispose()
if ($Image) { $bmp.Save((Join-Path (Get-Location) $Image), [System.Drawing.Imaging.ImageFormat]::Png) }

$sum = 0.0
$count = 0
$minL = 255.0
$maxL = 0.0
for ($y = 0; $y -lt $rh; $y += 2) {
    for ($x = 0; $x -lt ($rw - 1); $x++) {
        $a = $bmp.GetPixel($x, $y)
        $b = $bmp.GetPixel($x + 1, $y)
        $la = 0.299 * $a.R + 0.587 * $a.G + 0.114 * $a.B
        $lb = 0.299 * $b.R + 0.587 * $b.G + 0.114 * $b.B
        $sum += [Math]::Abs($la - $lb)
        $count++
        if ($la -lt $minL) { $minL = $la }
        if ($la -gt $maxL) { $maxL = $la }
    }
}
$bmp.Dispose()

$mean = if ($count -gt 0) { $sum / $count } else { 0.0 }
$lines = @(
    "rect=($rx,$ry) size=${rw}x${rh}",
    ("adjacent_contrast={0:F3}" -f $mean),
    ("luminance_min={0:F1} max={1:F1} range={2:F1}" -f $minL, $maxL, ($maxL - $minL)),
    ("verdict=" + $(if ($mean -lt 1.0) { 'BLURRED' } elseif ($mean -lt 6.0) { 'PARTIAL' } else { 'PATTERN_VISIBLE' }))
)
$lines | Set-Content -Path $Out -Encoding utf8
$lines | ForEach-Object { Write-Host $_ }
