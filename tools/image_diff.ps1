# Compare two PNGs pixel by pixel and report the difference.
#
# Used to prove hover magnification actually moved pixels: a still screenshot of a
# magnified dock looks plausible either way, so the evidence has to be a diff, not
# an opinion.
param(
    [Parameter(Mandatory = $true)][string]$A,
    [Parameter(Mandatory = $true)][string]$B,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Tolerance = 6
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Drawing

$bmpA = [System.Drawing.Bitmap]::FromFile((Resolve-Path $A))
$bmpB = [System.Drawing.Bitmap]::FromFile((Resolve-Path $B))

if ($bmpA.Width -ne $bmpB.Width -or $bmpA.Height -ne $bmpB.Height) {
    $msg = "size mismatch: $($bmpA.Width)x$($bmpA.Height) vs $($bmpB.Width)x$($bmpB.Height)"
    [System.IO.File]::WriteAllText($Out, $msg, (New-Object System.Text.UTF8Encoding($false)))
    $bmpA.Dispose(); $bmpB.Dispose()
    throw $msg
}

$w = $bmpA.Width
$h = $bmpA.Height
$changed = 0
$minX = $w; $minY = $h; $maxX = -1; $maxY = -1
# Row and column profiles: where the change is concentrated. A magnification shows
# up as a wide band in the row profile (icons grew upward) rather than a point.
$rowHits = New-Object int[] $h
$colHits = New-Object int[] $w

# LockBits is far faster than GetPixel for a 1510x213 image, but the interop for it
# needs unsafe code; 322k GetPixel calls take ~1s, which is acceptable for a probe.
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $pa = $bmpA.GetPixel($x, $y)
        $pb = $bmpB.GetPixel($x, $y)
        $d = [Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) + [Math]::Abs($pa.B - $pb.B)
        if ($d -gt $Tolerance) {
            $changed++
            $rowHits[$y]++
            $colHits[$x]++
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("a=$A")
$lines.Add("b=$B")
$lines.Add("size=${w}x${h}")
$lines.Add("tolerance=$Tolerance")
$lines.Add("changed_pixels=$changed")
$lines.Add("total_pixels=$($w * $h)")
$lines.Add("changed_pct=$([Math]::Round(100.0 * $changed / ($w * $h), 3))")
if ($changed -gt 0) {
    $lines.Add("bbox=$minX,$minY..$maxX,$maxY")
    $lines.Add("bbox_size=$($maxX - $minX + 1)x$($maxY - $minY + 1)")
    # First and last row that contain any change: the vertical extent of the effect.
    $firstRow = -1; $lastRow = -1
    for ($y = 0; $y -lt $h; $y++) {
        if ($rowHits[$y] -gt 0) { if ($firstRow -lt 0) { $firstRow = $y }; $lastRow = $y }
    }
    $lines.Add("rows_with_change=$firstRow..$lastRow")
    $firstCol = -1; $lastCol = -1
    for ($x = 0; $x -lt $w; $x++) {
        if ($colHits[$x] -gt 0) { if ($firstCol -lt 0) { $firstCol = $x }; $lastCol = $x }
    }
    $lines.Add("cols_with_change=$firstCol..$lastCol")
}
$lines.Add("verdict=$(if ($changed -gt 0) { 'DIFFERENT' } else { 'IDENTICAL' })")

$bmpA.Dispose()
$bmpB.Dispose()
$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
