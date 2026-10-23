# Verifies that a drag actually draws its two moving parts: the ghost and the
# insertion line.
#
# Both are transient - they exist only while the button is down - so the check has to
# compare a mid-drag screenshot against an idle reference taken from the same crop. A
# single screenshot cannot tell "the ghost is drawn" from "the desktop behind the dock
# happens to be bright there"; the diff can.
#
# Why the diff and not a raw pixel scan: the reserved band above the plate is
# transparent, so whatever wallpaper sits behind it shows through. Scanning for
# "non-black pixels" there measures the wallpaper. Diffing against idle removes it.
param(
    [Parameter(Mandatory = $true)][string]$Idle,
    [Parameter(Mandatory = $true)][string]$Drag,
    [Parameter(Mandatory = $true)][string]$Out,
    # Plate top in image pixels. Everything above is the reserved band, which is where
    # the lifted ghost is expected to be visible.
    [Parameter(Mandatory = $true)][int]$PlateTop,
    # Vertical band the insertion line occupies, in image pixels. Derived from the
    # painter's own geometry: the line is centred on the icon row and 0.62 icons tall.
    [Parameter(Mandatory = $true)][int]$LineTop,
    [Parameter(Mandatory = $true)][int]$LineBottom,
    # Pointer position in image pixels. The ghost is drawn centred here, so this is
    # where the changed pixels have to cluster. Without it the bbox also spans the
    # vacated source slot and the tool cannot tell "ghost at the pointer" from "ghost
    # stuck in its slot".
    [Parameter(Mandatory = $true)][int]$PointerX,
    [Parameter(Mandatory = $true)][int]$PointerY,
    # Half the ghost's box in pixels: the ghost is drawn at the idle icon size, so this
    # is icon_size * scale / 2.
    [Parameter(Mandatory = $true)][int]$GhostHalfPx,
    [int]$DiffTolerance = 18,
    [int]$BrightThreshold = 600
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Drawing

$lines = New-Object System.Collections.Generic.List[string]
function Add-Line { param([string]$Text) $lines.Add($Text); Write-Host $Text }

$a = [System.Drawing.Bitmap]::FromFile($Idle)
$b = [System.Drawing.Bitmap]::FromFile($Drag)
Add-Line "idle=$Idle"
Add-Line "drag=$Drag"
Add-Line "size=$($a.Width)x$($a.Height) plate_top=$PlateTop line_band=$LineTop..$LineBottom diff_tolerance=$DiffTolerance"

if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) {
    throw "screenshots differ in size: $($a.Width)x$($a.Height) vs $($b.Width)x$($b.Height)"
}

# --- ghost: changed pixels strictly above the plate ------------------------------
$minX = [int]::MaxValue; $maxX = -1; $minY = [int]::MaxValue; $maxY = -1; $changed = 0
for ($y = 0; $y -lt $PlateTop; $y++) {
    for ($x = 0; $x -lt $b.Width; $x++) {
        $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y)
        $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
        if ($d -gt $DiffTolerance) {
            $changed++
            if ($x -lt $minX) { $minX = $x }
            if ($x -gt $maxX) { $maxX = $x }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
}
# Where the changed pixels actually are, measured against the pointer. The ghost is
# centred on the pointer, so counting changed pixels inside a one-icon box around it
# separates "the ghost follows the pointer" from "the ghost stayed in its slot and the
# diff is just the row re-flowing".
$ghostOk = $false
if ($changed -gt 0) {
    $gw = $maxX - $minX + 1
    $gh = $maxY - $minY + 1
    Add-Line "ghost changed_pixels=$changed bbox=$minX,$minY..$maxX,$maxY size=${gw}x${gh}"

    $nearPointer = 0
    $half = [int]($GhostHalfPx)
    for ($y = [Math]::Max(0, $PointerY - $half); $y -le [Math]::Min($PlateTop - 1, $PointerY + $half); $y++) {
        for ($x = [Math]::Max(0, $PointerX - $half); $x -le [Math]::Min($b.Width - 1, $PointerX + $half); $x++) {
            $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y)
            $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
            if ($d -gt $DiffTolerance) { $nearPointer++ }
        }
    }
    Add-Line "ghost near_pointer=$nearPointer within=${half}px of ($PointerX,$PointerY)"
    # Thresholds scale with the ghost box. Fixed pixel counts would pass at 200% and fail
    # at 100% on the same rendering: at 96 DPI the whole ghost is 48 px tall and its
    # artwork only fills the lower part, so a 40 px height bar rejected a perfectly drawn
    # ghost. The bars are set to separate "a ghost is here" from "nothing changed", not
    # to grade the artwork.
    $minWidth = [int]($GhostHalfPx)
    $minHeight = [int]($GhostHalfPx * 0.8)
    $minNear = [int]($GhostHalfPx * $GhostHalfPx * 0.3)
    $ghostOk = ($gw -ge $minWidth) -and ($gh -ge $minHeight) -and ($nearPointer -ge $minNear)
    Add-Line "ghost thresholds width>=$minWidth height>=$minHeight near_pointer>=$minNear"
    Add-Line "ghost_verdict=$(if ($ghostOk) { 'DRAWN_AT_POINTER' } elseif ($gw -ge $minWidth -and $gh -ge $minHeight) { 'DRAWN_ELSEWHERE' } else { 'TOO_SMALL' })"
} else {
    Add-Line 'ghost changed_pixels=0'
    Add-Line 'ghost_verdict=MISSING'
}

# --- insertion line: a NARROW bright column that idle does not have ---------------
#
# Found rather than given an x, because the line is positioned from the magnified
# layout, which is a function of the pointer position - computing it here would mean
# reimplementing the layout engine in PowerShell, and a mistake there would produce a
# confident wrong answer.
#
# The discriminator is narrowness: the line is 3 DIP wide (6 px at 200%), while the
# narrowest icon is 48 DIP (96 px). A wide run of bright columns is an icon edge, not
# the line, so only runs of 10 px or fewer qualify.
$expected = $LineBottom - $LineTop + 1
$columnRows = @{}
for ($x = 0; $x -lt $b.Width; $x++) {
    $k = 0
    for ($y = $LineTop; $y -le $LineBottom; $y++) {
        $cb = $b.GetPixel($x, $y)
        $ca = $a.GetPixel($x, $y)
        if (($cb.R + $cb.G + $cb.B) -gt $BrightThreshold) {
            $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
            if ($d -gt 40) { $k++ }
        }
    }
    $columnRows[$x] = $k
}

$minRows = [int]($expected * 0.5)
$candidates = @()
$runStart = -1
for ($x = 0; $x -le $b.Width; $x++) {
    $tall = ($x -lt $b.Width) -and ($columnRows[$x] -ge $minRows)
    if ($tall -and $runStart -lt 0) { $runStart = $x }
    if (-not $tall -and $runStart -ge 0) {
        $runWidth = $x - $runStart
        $peak = 0
        for ($i = $runStart; $i -lt $x; $i++) { if ($columnRows[$i] -gt $peak) { $peak = $columnRows[$i] } }
        $candidates += [pscustomobject]@{ Left = $runStart; Width = $runWidth; PeakRows = $peak }
        $runStart = -1
    }
}

$narrow = @($candidates | Where-Object { $_.Width -le 10 })
$wide = @($candidates | Where-Object { $_.Width -gt 10 })
Add-Line "bright_runs total=$($candidates.Count) narrow=$($narrow.Count) wide=$($wide.Count) min_rows=$minRows of $expected"
foreach ($c in $narrow) {
    Add-Line "  narrow_run x=$($c.Left) width=$($c.Width) peak_rows=$($c.PeakRows)"
}
foreach ($c in $wide) {
    Add-Line "  wide_run x=$($c.Left) width=$($c.Width) peak_rows=$($c.PeakRows) (icon edge, ignored)"
}

$lineOk = $narrow.Count -ge 1
Add-Line "line_verdict=$(if ($lineOk) { 'DRAWN' } else { 'MISSING' })"

$verdict = if ($ghostOk -and $lineOk) { 'BOTH_DRAWN' } else { 'INCOMPLETE' }
Add-Line "verdict=$verdict"

$a.Dispose()
$b.Dispose()

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
[System.IO.File]::WriteAllText($Out, ($lines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
