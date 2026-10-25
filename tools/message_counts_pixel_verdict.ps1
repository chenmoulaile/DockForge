# T-065 pixel verdict: compare saved screenshots of the same dock slot.
#
# badge_on       - probe window "(4) badge probe" alive        -> badge must show
# badge_off      - probe window killed                        -> badge must be gone
# badge_reenabled - probe window alive again                  -> badge must show again
# badge_disabled - probe window alive, adapter switched off   -> badge must be gone
#
# "Badge" detection is a pixel diff inside the tile's top-right region, not a
# colour guess: the accent colour is whatever the system has, so the verdict is
# "the badge region changed vs the known-good no-badge frame", with the
# no-badge frames cross-checked against each other (they must agree).
#
# NOTE on variable naming: the parameters are $On/$Off/... and a PowerShell
# parameter carries its [string] type constraint on the VARIABLE, so reusing the
# same name (case-insensitively) for the loaded Bitmap silently converts it with
# ToString(). The images therefore live in $img* variables, never in $On.
param(
    [Parameter(Mandatory = $true)][string]$On,
    [Parameter(Mandatory = $true)][string]$Off,
    [Parameter(Mandatory = $true)][string]$Reenabled,
    [Parameter(Mandatory = $true)][string]$Disabled
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Load([string]$p) { [System.Drawing.Bitmap]::FromFile((Resolve-Path $p).Path) }

$imgOn = Load $On
$imgOff = Load $Off
$imgReen = Load $Reenabled
$imgDis = Load $Disabled

if ($imgOn.Width -ne $imgOff.Width -or $imgOn.Height -ne $imgOff.Height) {
    Write-Host "FAIL: screenshot sizes differ ($($imgOn.Width)x$($imgOn.Height) vs $($imgOff.Width)x$($imgOff.Height))"
    exit 1
}

# Region: the right half of the tile band. The dock is one slot wide, so the
# badge sits on the icon's top-right corner near the window's centre; a generous
# band from the centre-right down to just above the running indicator covers it
# at every DPI without touching the indicator.
$w = $imgOn.Width; $h = $imgOn.Height
$rx0 = [int]($w * 0.55); $rx1 = $w
$ry0 = [int]($h * 0.25); $ry1 = [int]($h * 0.75)

function DiffCount($a, $b) {
    $diff = 0
    for ($y = $ry0; $y -lt $ry1; $y++) {
        for ($x = $rx0; $x -lt $rx1; $x++) {
            $pa = $a.GetPixel($x, $y); $pb = $b.GetPixel($x, $y)
            $d = [Math]::Abs($pa.R - $pb.R) + [Math]::Abs($pa.G - $pb.G) +
                 [Math]::Abs($pa.B - $pb.B)
            if ($d -gt 60) { $diff++ }
        }
    }
    return $diff
}

$onVsOff = DiffCount $imgOn $imgOff
$reenVsOff = DiffCount $imgReen $imgOff
$disVsOff = DiffCount $imgDis $imgOff

Write-Host ("pixel diff in badge region (px with channel delta > 60):")
Write-Host ("  badge_on        vs badge_off : $onVsOff")
Write-Host ("  badge_reenabled vs badge_off : $reenVsOff")
Write-Host ("  badge_disabled  vs badge_off : $disVsOff")

$ok = $true
if ($onVsOff -lt 25) { Write-Host 'FAIL: badge_on does not differ from badge_off - no badge painted'; $ok = $false }
if ($reenVsOff -lt 25) { Write-Host 'FAIL: badge_reenabled does not differ from badge_off - badge did not come back'; $ok = $false }
if ($disVsOff -gt 15) { Write-Host 'FAIL: badge_disabled differs from badge_off - badge stayed after the switch'; $ok = $false }

$imgOn.Dispose(); $imgOff.Dispose(); $imgReen.Dispose(); $imgDis.Dispose()
if ($ok) {
    Write-Host 'pixel verdict: PASS (badge appears, exits to zero, switch disables)'
    exit 0
}
exit 1
