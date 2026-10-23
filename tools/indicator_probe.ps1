# Indicator dot probe: is there a running-indicator dot under each dock tile?
#
# Samples the indicator band inside the plate's bottom padding rather than the whole
# slot. The dot is 4 DIP wide and sits on a plate whose colour is known, so a single
# column of samples per tile is enough - and unlike a full-slot scan it cannot be
# confused by icon artwork that happens to reach the bottom edge.
#
# Exit verdict is a single line so callers can grep it:
#   verdict=DOTS_PRESENT|NO_DOTS|PARTIAL

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Count = 12,
    # Icon size and gap in DIP, matching config. Used to derive the row origin: the row
    # is centred in a window that is wider than the row (the extra width is headroom
    # for magnified neighbours), so tile centres are offset from the window edge.
    [double]$IconDip = 48.0,
    [double]$GapDip = 8.0,
    # -1 = derive from the image. Overriding is only needed when the dock is magnified,
    # which this probe deliberately does not handle: the indicator does not move on
    # hover, so probing the idle layout is the honest measurement.
    [double]$OriginDip = -1.0,
    [double]$FirstCenterDip = 0.0,   # 0 = origin + icon/2
    [double]$StrideDip = 0.0,        # 0 = icon + gap
    [double]$BandCenterDip = 0.0,    # 0 = derive from window height (height - 5 DIP)
    # Plate colour. 'auto' derives it from the band row itself (median of all pixels),
    # which is required rather than convenient: the solid material still blends 8% of
    # the backdrop, so the plate reads (13,15,18) over the desktop and (33,35,38) over a
    # white window. A hardcoded plate colour reports every tile as "has a dot" the
    # moment the backdrop changes - measured, and it made the exit test look broken.
    [string]$Plate = 'auto',
    [int]$Tolerance = 6,
    [int]$HalfWidthPx = 6
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing

$bitmap = [System.Drawing.Bitmap]::FromFile((Resolve-Path $Image).Path)
try {
    $scale = $bitmap.Height / 106.4
    # Row origin: the window is sized to hold the widest magnified row, which is wider
    # than the idle row, so the idle row is centred with padding on both sides. Using
    # `padding` as the origin (a tempting shortcut) puts every sample ~35 DIP left of
    # the icon it claims to measure - far enough to miss a 4 DIP dot entirely.
    $contentDip = $Count * $IconDip + [math]::Max(0, $Count - 1) * $GapDip
    $windowDip = $bitmap.Width / $scale
    $origin = if ($OriginDip -ge 0) { $OriginDip } else { ($windowDip - $contentDip) / 2.0 }
    $firstCenter = if ($FirstCenterDip -gt 0) { $FirstCenterDip } else { $origin + $IconDip / 2.0 }
    $stride = if ($StrideDip -gt 0) { $StrideDip } else { $IconDip + $GapDip }
    $bandY = if ($BandCenterDip -gt 0) { $BandCenterDip * $scale } else { ($bitmap.Height - 5.0 * $scale) }
    $bandRow = [int][math]::Round($bandY)

    # Derive the plate colour from the band row itself: the plate occupies most of that
    # row, so the mode is the plate and every dot is a minority. This is required, not
    # convenient - the solid material still blends 8% of the backdrop, so the plate
    # reads (13,15,18) over the desktop and (33,35,38) over a white window. A hardcoded
    # colour reported every tile as "has a dot" as soon as the backdrop changed.
    $plateParts = @(0, 0, 0)
    if ($Plate -eq 'auto') {
        if ($bandRow -lt 0 -or $bandRow -ge $bitmap.Height) {
            throw "band row $bandRow is outside the image"
        }
        $buckets = @{}
        for ($x = 0; $x -lt $bitmap.Width; $x += 2) {
            $p = $bitmap.GetPixel($x, $bandRow)
            $key = "$($p.R),$($p.G),$($p.B)"
            if ($buckets.ContainsKey($key)) { $buckets[$key]++ } else { $buckets[$key] = 1 }
        }
        $top = $buckets.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1
        $plateParts = @($top.Key.Split(',') | ForEach-Object { [int]$_ })
    } else {
        $plateParts = @($Plate.Split(',') | ForEach-Object { [int]$_.Trim() })
    }

    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("image=$Image")
    $lines.Add("size=$($bitmap.Width)x$($bitmap.Height) scale=$([math]::Round($scale,3))")
    $lines.Add("origin_dip=$([math]::Round($origin,2)) first_center_dip=$([math]::Round($firstCenter,2)) stride_dip=$([math]::Round($stride,2))")
    $lines.Add("band_y_px=$bandRow plate=$($plateParts -join ',') tolerance=$Tolerance")

    $withDot = 0
    for ($i = 0; $i -lt $Count; $i++) {
        $centerDip = $firstCenter + $i * $stride
        # Window DIP origin of the icon row is (width_dip - content_dip) / 2, which is
        # under a pixel for the default 12 tiles; sampling a +-6px window absorbs it.
        $cx = [int][math]::Round($centerDip * $scale)
        $cy = $bandRow

        $best = -1.0
        $bestX = -1
        $samples = 0
        for ($dx = -$HalfWidthPx; $dx -le $HalfWidthPx; $dx++) {
            $x = $cx + $dx
            if ($x -lt 0 -or $x -ge $bitmap.Width) { continue }
            if ($cy -lt 0 -or $cy -ge $bitmap.Height) { continue }
            $samples++
            $p = $bitmap.GetPixel($x, $cy)
            # Distance from the plate colour. The dot is near-white on a dark plate, so
            # any dot pixel is far from the plate in every channel.
            $d = [math]::Sqrt(
                [math]::Pow($p.R - $plateParts[0], 2) +
                [math]::Pow($p.G - $plateParts[1], 2) +
                [math]::Pow($p.B - $plateParts[2], 2))
            if ($d -gt $best) { $best = $d; $bestX = $x }
        }

        $present = $best -gt ($Tolerance * 2)
        if ($present) { $withDot++ }
        $lines.Add(("tile[{0}] cx={1} cy={2} samples={3} max_delta={4} at_x={5} dot={6}" -f `
                    $i, $cx, $cy, $samples, [math]::Round($best, 1), $bestX, $(if ($present) { 'YES' } else { 'NO' })))
    }

    $verdict = if ($withDot -eq 0) { 'NO_DOTS' } elseif ($withDot -eq $Count) { 'DOTS_PRESENT' } else { 'PARTIAL' }
    $lines.Add("tiles=$Count with_dot=$withDot")
    $lines.Add("verdict=$verdict")

    [System.IO.File]::WriteAllText($Out, ($lines -join "`n") + "`n",
        (New-Object System.Text.UTF8Encoding($false)))
    $lines | ForEach-Object { Write-Output $_ }
}
finally {
    $bitmap.Dispose()
}
