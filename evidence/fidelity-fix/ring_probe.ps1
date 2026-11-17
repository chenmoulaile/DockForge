# ADR-0058 evidence probe: ring opacity of a real shell outline composition vs a
# real full-bleed icon, per inset. Runs against the PNGs copied out of the icon
# cache; no DockForge process involved.
Add-Type -AssemblyName System.Drawing
foreach ($name in @('cache_12fad1fed92dd8c7_256.png', 'cache_0e273eecbc330788_256.png')) {
    $path = 'D:/MacDock/evidence/fidelity-fix/' + $name
    $bmp = New-Object System.Drawing.Bitmap($path)
    Write-Output ("== {0}  {1}x{2}" -f $name, $bmp.Width, $bmp.Height)
    for ($inset = 0; $inset -lt 12; $inset++) {
        $total = 0
        $opaque = 0
        $top = $bmp.Height - 1 - $inset
        for ($x = $inset; $x -lt $bmp.Width - $inset; $x++) {
            $a1 = $bmp.GetPixel($x, $inset).A
            $a2 = $bmp.GetPixel($x, $top).A
            $total += 2
            if ($a1 -gt 40) { $opaque++ }
            if ($a2 -gt 40) { $opaque++ }
        }
        $right = $bmp.Width - 1 - $inset
        for ($y = $inset; $y -lt $bmp.Height - $inset; $y++) {
            $a1 = $bmp.GetPixel($inset, $y).A
            $a2 = $bmp.GetPixel($right, $y).A
            $total += 2
            if ($a1 -gt 40) { $opaque++ }
            if ($a2 -gt 40) { $opaque++ }
        }
        Write-Output ("ring inset={0} opacity={1}" -f $inset, [math]::Round($opaque / $total, 3))
    }
    $bmp.Dispose()
}
