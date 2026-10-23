# tools/sample_pixels.ps1 - sample specific screen pixels into a text report.
# Used by rendering spikes to prove "painted here, transparent there" without
# relying on the eye. Captures one screen grab and reads several points from it.
#
#   pwsh -File tools/sample_pixels.ps1 -Out report.txt `
#       -Rect 120,140,480,180 -Points "center:240,90;corner:6,6"
#
# -Rect is "x,y,w,h" in physical pixels; -Points is "name:x,y" pairs relative to
# the rect origin.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Rect,
    [Parameter(Mandatory = $true)][string]$Points,
    [string]$Image = ''
)

Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Dpi2 {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    public static void Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
}
'@
[Dpi2]::Enable()

$parts = $Rect.Split(',')
$rx = [int]$parts[0]; $ry = [int]$parts[1]; $rw = [int]$parts[2]; $rh = [int]$parts[3]

$bmp = New-Object System.Drawing.Bitmap($rw, $rh)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($rx, $ry, 0, 0, (New-Object System.Drawing.Size($rw, $rh)))
$g.Dispose()

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("rect=($rx,$ry) size=${rw}x${rh}")

foreach ($pt in $Points.Split(';')) {
    if (-not $pt) { continue }
    $kv = $pt.Split(':')
    $name = $kv[0]
    $xy = $kv[1].Split(',')
    $x = [int]$xy[0]; $y = [int]$xy[1]
    if ($x -lt 0 -or $y -lt 0 -or $x -ge $rw -or $y -ge $rh) {
        $lines.Add(("{0,-14} ({1},{2}) OUT_OF_RECT" -f $name, $x, $y))
        continue
    }
    $c = $bmp.GetPixel($x, $y)
    $lines.Add(("{0,-14} ({1},{2}) rgba=({3},{4},{5},{6})" -f $name, $x, $y, $c.R, $c.G, $c.B, $c.A))
}

if ($Image) {
    $dir = Split-Path -Parent $Image
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $bmp.Save((Join-Path (Get-Location) $Image), [System.Drawing.Imaging.ImageFormat]::Png)
    $lines.Add("image=$Image")
}

$bmp.Dispose()
$dir2 = Split-Path -Parent $Out
if ($dir2 -and -not (Test-Path $dir2)) { New-Item -ItemType Directory -Force -Path $dir2 | Out-Null }
$lines | Set-Content -LiteralPath $Out -Encoding UTF8
$lines | ForEach-Object { Write-Output $_ }
