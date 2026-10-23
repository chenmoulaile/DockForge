# Report the vertical extent of non-plate pixels along one column.
#
# The dock's magnification is a change in icon HEIGHT, so the honest measurement is
# "how many pixels tall is this icon now" rather than "did anything change". A
# screenshot of a magnified dock looks plausible at any scale; this gives a number.
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][int[]]$Columns,
    [string]$Plate = '13,15,18',
    [int]$Tolerance = 12,
    [int]$Top = 0,
    [int]$Bottom = -1,
    # Second ignored colour: the desktop showing through the transparent band above
    # the plate. Without it the band counts as content and every column reads full
    # height.
    [string]$AlsoIgnore = '0,0,0'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Drawing

$plateParts = $Plate -split ','
$pr = [int]$plateParts[0]
$pg = [int]$plateParts[1]
$pb = [int]$plateParts[2]

$ignoreParts = $AlsoIgnore -split ','
$ir = [int]$ignoreParts[0]
$ig = [int]$ignoreParts[1]
$ib = [int]$ignoreParts[2]

$bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Image))
if ($Bottom -lt 0) { $Bottom = $bmp.Height - 1 }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("image=$Image")
$lines.Add("size=$($bmp.Width)x$($bmp.Height)")
$lines.Add("plate=$Plate tolerance=$Tolerance scan_rows=$Top..$Bottom")

foreach ($x in $Columns) {
    if ($x -lt 0 -or $x -ge $bmp.Width) {
        $lines.Add("col=$x OUT_OF_IMAGE")
        continue
    }
    $first = -1
    $last = -1
    $count = 0
    for ($y = $Top; $y -le $Bottom; $y++) {
        $p = $bmp.GetPixel($x, $y)
        $d = [Math]::Abs($p.R - $pr) + [Math]::Abs($p.G - $pg) + [Math]::Abs($p.B - $pb)
        $d2 = [Math]::Abs($p.R - $ir) + [Math]::Abs($p.G - $ig) + [Math]::Abs($p.B - $ib)
        if ($d -gt $Tolerance -and $d2 -gt $Tolerance) {
            $count++
            if ($first -lt 0) { $first = $y }
            $last = $y
        }
    }
    if ($first -lt 0) {
        $lines.Add("col=$x extent=NONE")
    } else {
        $lines.Add("col=$x extent=$first..$last height=$($last - $first + 1) non_plate_px=$count")
    }
}

$bmp.Dispose()
$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
