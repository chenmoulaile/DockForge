# DockForge - per-slot icon presence check.
#
# Answers one question with evidence instead of eyeballing a screenshot: does each
# dock slot actually contain a rendered icon, and is it where the layout says it is?
# A slot that reads as plate colour everywhere is a missing or mispositioned icon,
# which a single centre sample would misreport for icons with a dark centre.
#
# Usage:
#   .\tools\dock_slots.ps1 -Image evidence\T-009\dpi192.png -Out report.txt `
#       -Count 12 -FirstCenter 68 -Stride 112 -HalfSize 48 -Plate 13,15,18
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Count = 12,
    [int]$FirstCenter = 68,
    [int]$Stride = 112,
    [int]$CenterY = 68,
    [int]$HalfSize = 48,
    [string]$Plate = "13,15,18",
    [int]$Tolerance = 6
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Drawing

$plateParts = $Plate -split ',' | ForEach-Object { [int]$_ }
$plateR = $plateParts[0]
$plateG = $plateParts[1]
$plateB = $plateParts[2]

$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Image))
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("source=image $Image size=$($bmp.Width)x$($bmp.Height)")
$lines.Add("plate_rgb=$plateR,$plateG,$plateB tolerance=$Tolerance")
$lines.Add(("{0,-6} {1,-10} {2,-10} {3,-8} {4,-8} {5}" -f 'slot', 'center', 'drawn_px', 'coverage', 'verdict', 'bbox'))

$totalDrawn = 0
$slotsWithIcon = 0
for ($i = 0; $i -lt $Count; $i++) {
    $cx = $FirstCenter + $i * $Stride
    $x0 = [Math]::Max(0, $cx - $HalfSize)
    $x1 = [Math]::Min($bmp.Width - 1, $cx + $HalfSize)
    $y0 = [Math]::Max(0, $CenterY - $HalfSize)
    $y1 = [Math]::Min($bmp.Height - 1, $CenterY + $HalfSize)

    $drawn = 0
    $sampled = 0
    $minX = [int]::MaxValue; $maxX = -1; $minY = [int]::MaxValue; $maxY = -1
    # 2px step: enough to characterise an icon without reading a megapixel per slot.
    for ($y = $y0; $y -le $y1; $y += 2) {
        for ($x = $x0; $x -le $x1; $x += 2) {
            $c = $bmp.GetPixel($x, $y)
            $sampled++
            if ([Math]::Abs($c.R - $plateR) -le $Tolerance -and
                [Math]::Abs($c.G - $plateG) -le $Tolerance -and
                [Math]::Abs($c.B - $plateB) -le $Tolerance) { continue }
            $drawn++
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
    $coverage = if ($sampled -gt 0) { [Math]::Round(100.0 * $drawn / $sampled, 1) } else { 0.0 }
    # Below ~2% coverage the slot is plate, not an icon. Measured: real icons land
    # far above that (they fill most of the slot), a blank slot lands at 0.
    $verdict = if ($drawn -eq 0) { 'EMPTY' } elseif ($coverage -lt 2.0) { 'SPARSE' } else { 'ICON' }
    if ($verdict -eq 'ICON') { $slotsWithIcon++ }
    $totalDrawn += $drawn
    $bbox = if ($maxX -ge 0) { "$minX,$minY-$maxX,$maxY" } else { '-' }
    $lines.Add(("{0,-6} {1,-10} {2,-10} {3,-8} {4,-8} {5}" -f $i, "$cx,$CenterY", $drawn, "$coverage%", $verdict, $bbox))
}
$bmp.Dispose()

$lines.Add("slots=$Count with_icon=$slotsWithIcon total_drawn_samples=$totalDrawn")
$lines.Add("verdict=$(if ($slotsWithIcon -eq $Count) { 'ALL_SLOTS_DRAWN' } else { 'MISSING_ICONS' })")

$text = $lines -join "`r`n"
[System.IO.File]::WriteAllText($Out, $text + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Output $text
