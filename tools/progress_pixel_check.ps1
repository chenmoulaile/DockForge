# T-067 evidence helper: verify the progress bar's pixels on screen.
#
# The bar's Normal colour is the system accent (C-22). This script reads the
# accent from the registry the same way the product does (AccentColor, then the
# Win11 Accent\AccentColorMenu default), then compares the on/off screenshots:
# the bar is the pixel population that exists ONLY while the copy ran.
param(
    [string]$PngOn = 'evidence\T-067\progress_on.png',
    [string]$PngOff = 'evidence\T-067\progress_off.png'
)
Add-Type -AssemblyName System.Drawing

$accent = $null
try {
    $accent = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer' -ErrorAction Stop).AccentColor
} catch {}
if (-not $accent) {
    try {
        $accent = (Get-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\Accent' -ErrorAction Stop).AccentColorMenu
    } catch {}
}
if (-not $accent) { $accent = 0xFF0078D4 }
$r = $accent -band 0xFF; $g = ($accent -shr 8) -band 0xFF; $b = ($accent -shr 16) -band 0xFF
Write-Host ("accent colour: R=$r G=$g B=$b (0x{0:X6})" -f ((($r -shl 16) -bor ($g -shl 8) -bor $b)))

$on = [System.Drawing.Bitmap]::FromFile((Resolve-Path $PngOn))
$off = [System.Drawing.Bitmap]::FromFile((Resolve-Path $PngOff))
try {
    # Pixels within tolerance of the accent that exist in ON but not OFF.
    $extraRows = @{}
    $total = 0
    for ($y = 0; $y -lt $on.Height; ++$y) {
        $rowExtra = 0
        for ($x = 0; $x -lt $on.Width; ++$x) {
            $a = $on.GetPixel($x, $y)
            $c = $off.GetPixel($x, $y)
            $aIsAccent = ([Math]::Abs($a.R - $r) -le 45 -and [Math]::Abs($a.G - $g) -le 45 -and
                [Math]::Abs($a.B - $b) -le 45)
            $cIsAccent = ([Math]::Abs($c.R - $r) -le 45 -and [Math]::Abs($c.G - $g) -le 45 -and
                [Math]::Abs($c.B - $b) -le 45)
            if ($aIsAccent -and -not $cIsAccent) { ++$rowExtra; ++$total }
        }
        if ($rowExtra -gt 0) { $extraRows[$y] = $rowExtra }
    }
    Write-Host ("pixels matching accent ONLY while the copy ran: $total")
    if ($total -gt 0) {
        $ys = $extraRows.Keys | Sort-Object
        Write-Host ("rows: y=$($ys[0])..$($ys[-1]) ($($ys.Count) rows), per-row counts: " +
            (($ys | ForEach-Object { "$_=$($extraRows[$_])" }) -join ' '))
        $height = $ys[-1] - $ys[0] + 1
        Write-Host ("bar height in px: $height (4 DIP at the screen's DPI scale)")
        if ($total -ge 40) {
            Write-Host 'VERDICT: PASS - an accent-coloured bar existed only during the copy'
        } else {
            Write-Host 'VERDICT: WEAK - too few pixels to call it a bar'
        }
    } else {
        Write-Host 'VERDICT: FAIL - no accent population difference between on/off'
    }
} finally {
    $on.Dispose(); $off.Dispose()
}
