# Verifies that the external-drop caret is drawn, and where.
#
# Why this is not just an image diff: the caret is 4 DIP wide and about 30 DIP tall, so
# at 96 DPI it is a 4x30 patch - a whole-frame diff reports it as 0.16% of the image and
# its bounding box is dominated by whatever else moved (the row settling, an icon
# highlight). The honest question is not "did anything change" but "is there a narrow
# vertical column, tall enough to be the caret, that was not there while idle".
#
# A column qualifies when it changed along most of the caret band AND is at most
# MaxCaretWidthPx wide. Width is the discriminator: an icon edge is 48 DIP wide, a
# settled row shifts hundreds of pixels, and only the caret is a few pixels across.
param(
    [Parameter(Mandatory = $true)][string]$Idle,
    [Parameter(Mandatory = $true)][string]$Drag,
    [Parameter(Mandatory = $true)][string]$Out,
    # Vertical band the caret occupies, in image pixels. The caret is centred on the icon
    # row and 0.62 icons tall, so this is derived from the dock's own geometry.
    [Parameter(Mandatory = $true)][int]$BandTop,
    [Parameter(Mandatory = $true)][int]$BandBottom,
    # Per-channel sum threshold. 30 ignores JPEG-ish noise and the plate's own gradient.
    [int]$DiffTolerance = 30,
    # A caret column must change along at least this fraction of the band.
    [double]$MinColumnCoverage = 0.6,
    # Widest a run of qualifying columns may be before it is "something else".
    [int]$MaxCaretWidthPx = 12,
    # Expected x, in image pixels, from the geometry. Checked, not assumed: a caret at
    # the wrong slot is worse than no caret, because it promises the wrong landing spot.
    [int]$ExpectedX = -1,
    [int]$ExpectedTolerancePx = 14,
    # 'accept', 'reject', or '' to skip. A caret in the wrong colour is worse than a
    # missing one: it tells the user the payload will be taken and then refuses it.
    [string]$ExpectedTint = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Drawing

$lines = New-Object System.Collections.Generic.List[string]
function Add-Line { param([string]$Text) $lines.Add($Text); Write-Host $Text }

$a = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Idle))
$b = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Drag))
Add-Line "idle=$Idle"
Add-Line "drag=$Drag"
Add-Line "size=$($b.Width)x$($b.Height) band=$BandTop..$BandBottom tolerance=$DiffTolerance coverage>=$MinColumnCoverage max_width=$MaxCaretWidthPx"

if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) {
    throw "screenshots differ in size: $($a.Width)x$($a.Height) vs $($b.Width)x$($b.Height)"
}
if ($BandBottom -le $BandTop) { throw "empty band: $BandTop..$BandBottom" }

$bandHeight = $BandBottom - $BandTop + 1
$need = [int][Math]::Ceiling($bandHeight * $MinColumnCoverage)

# Per column: how many rows in the band changed, the mean colour of those pixels, and the
# single strongest change with its colour.
#
# Three numbers because each answers a different question. Coverage says "the caret is
# here". The strongest change says "this is the caret's true colour" - the caret is
# anti-aliased against the plate, so its edge columns blend toward the background and
# reporting the run's first column made the reject caret read as (46,28,29), i.e. almost
# black, and the tint test called it accept. The mean is kept for the log because it makes
# a partially covered run obvious.
$qualifying = New-Object System.Collections.Generic.List[object]
for ($x = 0; $x -lt $b.Width; $x++) {
    $changed = 0
    $sumR = 0.0; $sumG = 0.0; $sumB = 0.0
    $peak = 0
    $peakR = 0; $peakG = 0; $peakB = 0
    for ($y = $BandTop; $y -le $BandBottom; $y++) {
        $ca = $a.GetPixel($x, $y); $cb = $b.GetPixel($x, $y)
        $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
        if ($d -gt $DiffTolerance) {
            $changed++
            $sumR += $cb.R; $sumG += $cb.G; $sumB += $cb.B
            if ($d -gt $peak) { $peak = $d; $peakR = $cb.R; $peakG = $cb.G; $peakB = $cb.B }
        }
    }
    if ($changed -ge $need) {
        $qualifying.Add([pscustomobject]@{
            x = $x; changed = $changed; peak = $peak
            r = [int][Math]::Round($sumR / $changed)
            g = [int][Math]::Round($sumG / $changed)
            b = [int][Math]::Round($sumB / $changed)
            pr = $peakR; pg = $peakG; pb = $peakB
        })
    }
}
$a.Dispose(); $b.Dispose()

Add-Line "qualifying_columns=$($qualifying.Count) need_rows=$need of $bandHeight"

# Group adjacent columns into runs. A caret is one run; two runs means something else
# changed as well and the verdict has to say so rather than pick the prettier one.
$runs = New-Object System.Collections.Generic.List[object]
if ($qualifying.Count -gt 0) {
    $start = $qualifying[0].x; $prev = $qualifying[0].x
    $best = $qualifying[0]
    for ($i = 1; $i -lt $qualifying.Count; $i++) {
        $q = $qualifying[$i]
        if ($q.x -ne $prev + 1) {
            $runs.Add([pscustomobject]@{ left = $start; right = $prev; r = $best.r; g = $best.g
                                         b = $best.b; pr = $best.pr; pg = $best.pg; pb = $best.pb
                                         peak = $best.peak })
            $start = $q.x; $best = $q
        } elseif ($q.peak -gt $best.peak) {
            # The run's colour is its most-changed column: the one the caret's fill
            # actually landed on, not the anti-aliased fringe.
            $best = $q
        }
        $prev = $q.x
    }
    $runs.Add([pscustomobject]@{ left = $start; right = $prev; r = $best.r; g = $best.g
                                 b = $best.b; pr = $best.pr; pg = $best.pg; pb = $best.pb
                                 peak = $best.peak })
}

foreach ($run in $runs) {
    $width = $run.right - $run.left + 1
    Add-Line ("run x={0}..{1} width={2} mean=({3},{4},{5}) peak=({6},{7},{8}) peak_delta={9}" -f
              $run.left, $run.right, $width, $run.r, $run.g, $run.b, $run.pr, $run.pg, $run.pb,
              $run.peak)
}

$narrow = @($runs | Where-Object { ($_.right - $_.left + 1) -le $MaxCaretWidthPx })
$verdict = 'NOT_DRAWN'
if ($narrow.Count -eq 0) {
    $verdict = 'NOT_DRAWN'
} elseif ($narrow.Count -gt 1) {
    $verdict = 'AMBIGUOUS'
} else {
    $caret = $narrow[0]
    $width = $caret.right - $caret.left + 1
    $centre = ($caret.left + $caret.right) / 2.0
    # Warm red vs near-white is how the accept and reject carets differ (C-16e). Judged on
    # the peak pixel: the fill colour is the one the painter asked for, and the fringe
    # blends it toward the plate. Red channel leading blue by 40 is the boundary - the
    # measured fills are (212,218,227) and (219,98,89), so the two are nowhere near it.
    $tint = if ($caret.pr -gt ($caret.pb + 40)) { 'reject' } else { 'accept' }
    Add-Line ("caret x={0}..{1} width={2} centre={3} tint={4}" -f $caret.left, $caret.right,
              $width, $centre, $tint)
    if ($ExpectedX -ge 0) {
        $delta = [Math]::Abs($centre - $ExpectedX)
        Add-Line ("expected_x={0} delta={1} tolerance={2}" -f $ExpectedX, $delta, $ExpectedTolerancePx)
        $verdict = if ($delta -le $ExpectedTolerancePx) { 'DRAWN_AT_EXPECTED_SLOT' } else { 'DRAWN_AT_WRONG_SLOT' }
    } else {
        $verdict = 'DRAWN'
    }
    if ($ExpectedTint) {
        $tintOk = ($tint -eq $ExpectedTint)
        Add-Line ("expected_tint={0} actual_tint={1}" -f $ExpectedTint, $tint)
        if (-not $tintOk) { $verdict = 'WRONG_TINT' }
    }
}
Add-Line "verdict=$verdict"

[System.IO.File]::WriteAllText($Out, ($lines -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
