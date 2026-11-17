# Crop the widget-zone close-ups out of the saved top strips for visual review.
Add-Type -AssemblyName System.Drawing
$repo = Split-Path -Parent $PSScriptRoot
foreach ($m in @("grade_normal", "grade_warn", "grade_critical", "default")) {
    $srcPath = Join-Path $repo "evidence\T-302\widgets_$m`_strip.png"
    if (-not (Test-Path $srcPath)) { continue }
    $src = [System.Drawing.Image]::FromFile($srcPath)
    $crop = New-Object System.Drawing.Bitmap 900, 56
    $g = [System.Drawing.Graphics]::FromImage($crop)
    $x = [Math]::Max(0, [int](($src.Width - 900) / 2))
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, 900, 56),
                 (New-Object System.Drawing.Rectangle $x, 0, 900, 56),
                 [System.Drawing.GraphicsUnit]::Pixel)
    $crop.Save((Join-Path $repo "evidence\T-302\zoom_$m.png"))
    # The temperature box is the row's rightmost entry; a right-anchored crop
    # keeps it fully inside the frame for the per-tier visual review.
    $xr = [Math]::Max(0, $src.Width - 900)
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle 0, 0, 900, 56),
                 (New-Object System.Drawing.Rectangle $xr, 0, 900, 56),
                 [System.Drawing.GraphicsUnit]::Pixel)
    $crop.Save((Join-Path $repo "evidence\T-302\zoom_${m}_right.png"))
    $g.Dispose(); $crop.Dispose(); $src.Dispose()
    Write-Host "saved zoom_$m.png + zoom_${m}_right.png"
}
