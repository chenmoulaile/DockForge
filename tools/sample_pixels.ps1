# tools/sample_pixels.ps1 - sample specific pixels into a text report.
# Used by rendering spikes to prove "painted here, transparent there" without
# relying on the eye.
#
# Two modes, and the difference matters:
#   -Image given  -> reads THAT file and samples it. -Rect is a crop offset
#                    inside the image (default "0,0,<w>,<h>"), so the numbers
#                    describe the artifact you are about to cite as evidence.
#   no -Image     -> grabs the live screen at -Rect and samples that.
#
# Sampling the live screen while a file of the same name sits on disk is how
# fake evidence gets made: the file says one thing, the report says another.
# Pass -Image whenever the report is meant to describe a saved capture.
#
#   pwsh -File tools/sample_pixels.ps1 -Out report.txt -Image shot.png `
#       -Rect 0,0,528,152 -Points "center:264,76;corner:6,6"
#   pwsh -File tools/sample_pixels.ps1 -Out report.txt `
#       -Rect 1176,836,528,152 -Points "center:264,76"
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Points,
    # "x,y,w,h" in physical pixels. With -Image this is an offset inside the file.
    [string]$Rect = '',
    # Existing PNG to read instead of grabbing the screen.
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

$lines = New-Object System.Collections.Generic.List[string]

if ($Image) {
    if (-not (Test-Path -LiteralPath $Image)) {
        Write-Error "image not found: $Image"
        exit 1
    }
    $src = [System.Drawing.Bitmap]::FromFile((Resolve-Path -LiteralPath $Image).Path)
    if (-not $Rect) { $Rect = "0,0,$($src.Width),$($src.Height)" }
    $parts = $Rect.Split(',')
    $rx = [int]$parts[0]; $ry = [int]$parts[1]; $rw = [int]$parts[2]; $rh = [int]$parts[3]
    $bmp = New-Object System.Drawing.Bitmap($rw, $rh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $rw, $rh)),
                 (New-Object System.Drawing.Rectangle($rx, $ry, $rw, $rh)),
                 [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $src.Dispose()
    $lines.Add("source=image $Image rect=($rx,$ry) size=${rw}x${rh}")
} else {
    # Live grab: only correct while the app is actually on screen.
    [Dpi2]::Enable()
    if (-not $Rect) { Write-Error "-Rect is required when -Image is not given"; exit 1 }
    $parts = $Rect.Split(',')
    $rx = [int]$parts[0]; $ry = [int]$parts[1]; $rw = [int]$parts[2]; $rh = [int]$parts[3]
    $bmp = New-Object System.Drawing.Bitmap($rw, $rh)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($rx, $ry, 0, 0, (New-Object System.Drawing.Size($rw, $rh)))
    $g.Dispose()
    $lines.Add("source=screen rect=($rx,$ry) size=${rw}x${rh}")
}

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

$bmp.Dispose()
$dir2 = Split-Path -Parent $Out
if ($dir2 -and -not (Test-Path $dir2)) { New-Item -ItemType Directory -Force -Path $dir2 | Out-Null }
$lines | Set-Content -LiteralPath $Out -Encoding UTF8
$lines | ForEach-Object { Write-Output $_ }
