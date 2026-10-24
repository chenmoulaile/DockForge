# T-016 evidence: hover-dwell preview against the live dock.
#
# Every assertion reads the app's own log. A screenshot that "looks empty" cannot
# distinguish "no preview was shown" from "the preview was shown somewhere else",
# and T-015 already proved the sampling lesson once: single-point sampling tells
# you nothing about where a thing was drawn. So the probe drives the app, waits on
# the log line it emits, and only then captures.
#
# Cases (mirrors C-17j and TASKS.md T-016):
#   1. dwell < 400ms  -> no "preview shown tile=" line at all
#   2. dwell >= 400ms -> the line appears, with the hovered tile index
#   3. leaving the dock -> a "preview hidden" line, and a second dwell re-arms
#   4. screenshot taken AFTER the log line, so the shutter cannot lead the event
#
# WHY THE HOVER COMES FROM A FILE, NOT FROM SetCursorPos
# The first version of this probe warped the real cursor onto a tile and held it
# there. That cannot work on a machine a human is using: measured 2026-10-24, the
# physical mouse crossed the dock mid-case and the dwell went 0 -> 125.9ms -> 0,
# and every SetCursorPos warp was overwritten within one frame. The dwell was
# accumulating correctly - the instrument was the problem, not the product.
#
# DOCKFORGE_HOVER_PROBE_FILE points at a file holding a decimal tile index (or -1
# for "no hover"). The app re-reads it at most every 50ms and substitutes only the
# *source* of the hover; HitTest on the real path, the 400ms dwell timer, the
# window lookup, the popup placement and every DwmRegisterThumbnail call still run
# production code. This is the same kind of hook as DOCKFORGE_FORCE_MATERIAL and
# DOCKFORGE_DPI_OVERRIDE, and it is off unless the variable is set.
param(
    [string]$OutDir = 'evidence\T-016',
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$Dpi = 192,
    [int]$SettleMs = 3000,
    # Must be below kPreviewDwellMs (400). 250ms keeps the target well inside the
    # latch while still being far under it.
    [int]$ShortDwellMs = 250,
    [int]$LongDwellMs = 900,
    # Restricts the dock to apps whose display name contains this substring, so the
    # probe can aim at a tile that is guaranteed to have a live window. The default
    # dock row is built from the user's config and contains mostly apps that are
    # not running, and "dwell elapsed on tile N with no window" is a correct
    # outcome for those - it cannot prove the thumbnail chain.
    [string]$PinApps = '',
    [string]$ConfigBackup = '',
    # Extra case: after a popup has been shown and hidden, minimize every source window
    # and dwell again. DWM keeps the last composed frame for a minimized window, so the
    # slot must still show the old contents rather than a black rectangle. Proving that
    # needs the popup to be raised a second time with the windows already minimized.
    [switch]$MinimizeSource,
    [string]$MinimizeProcess = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
$configPath = Join-Path $env:APPDATA 'DockForge\config.json'
$probeFile = Join-Path $env:TEMP 'dockforge_hover_probe.txt'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T016Win {
    // Without PER_MONITOR_AWARE_V2 this process is DPI-unaware, and BOTH
    // GetWindowRect and CopyFromScreen silently return virtualised coordinates:
    // measured on the 192 DPI machine, a dock at 1334,1595 214x214 was reported as
    // 667,798 107x107, so every capture landed on the wrong half of the screen.
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@ -ErrorAction SilentlyContinue
# -4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, the same context the app sets.
[void][T016Win]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-LatestLog {
    Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

# The app holds the log open for appending, so plain ReadAllText fails on share
# mode. Open explicitly with ReadWrite sharing, then read from an offset.
function Read-LogFrom([long]$offset) {
    $log = Get-LatestLog
    if ($log -eq $null) { return '' }
    $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        if ($offset -gt $fs.Length) { $offset = 0 }
        $fs.Position = $offset
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        return $sr.ReadToEnd()
    } finally {
        $fs.Close()
    }
}

function Get-LogLength {
    $log = Get-LatestLog
    if ($log -eq $null) { return 0 }
    return $log.Length
}

function Get-DockRect {
    $target = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $target) { return $null }
    $cb = [T016Win+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T016Win]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $target.Id) { return $true }
        $sb = New-Object System.Text.StringBuilder 256
        [void][T016Win]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:found = $h; return $false }
        return $true
    }
    $script:found = [IntPtr]::Zero
    [void][T016Win]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:found -eq [IntPtr]::Zero) { return $null }
    $r = New-Object T016Win+RECT
    [void][T016Win]::GetWindowRect($script:found, [ref]$r)
    return $r
}

# Every visible top-level window owned by any of $Pids. Used by the minimized-source
# case: an app with four windows exposes only one MainWindowHandle, and minimizing just
# that one would leave three slots showing their visible frames.
function Get-TopLevelWindows([int[]]$Pids) {
    $set = @{}
    foreach ($id in $Pids) { $set[[uint32]$id] = $true }
    $found = New-Object 'System.Collections.Generic.List[IntPtr]'
    $cb = [T016Win+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T016Win]::GetWindowThreadProcessId($h, [ref]$procId)
        if (-not $set.ContainsKey($procId)) { return $true }
        if (-not [T016Win]::IsWindowVisible($h)) { return $true }
        $found.Add($h)
        return $true
    }
    [void][T016Win]::EnumWindows($cb, [IntPtr]::Zero)
    return $found.ToArray()
}

# Publish the hover state the app polls. Written atomically enough for this use:
# a torn read can only produce "no hover" for one poll, and the next poll 50ms
# later sees the settled value.
function Set-HoverProbe([int]$Tile) {
    [System.IO.File]::WriteAllText($probeFile, "$Tile",
        (New-Object System.Text.UTF8Encoding($false)))
}

# Wait until the log grows past $offset and contains $pattern, or give up.
function Wait-LogLine([long]$offset, [string]$pattern, [int]$TimeoutMs) {
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $tail = Read-LogFrom $offset
        if ($tail) {
            foreach ($line in ($tail -split "`n")) {
                if ($line -match $pattern) { return $line.TrimEnd() }
            }
        }
        Start-Sleep -Milliseconds 40
    }
    return $null
}

function Save-Shot([string]$Path, [int]$Left, [int]$Top, [int]$Width, [int]$Height) {
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap($Width, $Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($Left, $Top, 0, 0, (New-Object System.Drawing.Size($Width, $Height)))
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

# Copies a sub-rectangle out of a saved shot. Used to re-crop the control and visible
# frames to the popup's own rectangle before diffing, so the ratio measures the popup
# rather than the empty desktop the wide capture region also contains.
function Save-Crop([string]$SourcePath, [string]$Path, [int]$Left, [int]$Top, [int]$Width, [int]$Height) {
    Add-Type -AssemblyName System.Drawing
    $src = New-Object System.Drawing.Bitmap($SourcePath)
    $dst = New-Object System.Drawing.Bitmap($Width, $Height)
    $g = [System.Drawing.Graphics]::FromImage($dst)
    $g.DrawImage($src, (New-Object System.Drawing.Rectangle(0, 0, $Width, $Height)),
        (New-Object System.Drawing.Rectangle($Left, $Top, $Width, $Height)),
        [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $dst.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $dst.Dispose(); $src.Dispose()
}

# Pixel statistics for a saved shot. A colour count alone proves nothing - the
# desktop behind the popup is also colourful - so the two numbers that matter are
# compared against a control frame taken at the same rectangle with the popup
# hidden (see the diff below).
function Get-ShotStats([string]$Path) {
    Add-Type -AssemblyName System.Drawing
    # $Path -> $bmp: never name a local the same as a parameter. PowerShell is
    # case-insensitive, so $p would BE the [string]$Path and stay a string.
    $bmp = New-Object System.Drawing.Bitmap($Path)
    $colors = New-Object 'System.Collections.Generic.HashSet[string]'
    $sum = 0.0; $n = 0; $max = 0
    for ($y = 0; $y -lt $bmp.Height; $y += 2) {
        for ($x = 0; $x -lt $bmp.Width; $x += 2) {
            $c = $bmp.GetPixel($x, $y)
            $l = [int](($c.R + $c.G + $c.B) / 3)
            $sum += $l; $n++
            if ($l -gt $max) { $max = $l }
            [void]$colors.Add("$($c.R),$($c.G),$($c.B)")
        }
    }
    $bmp.Dispose()
    return [pscustomobject]@{
        Samples  = $n
        MeanLuma = [Math]::Round($sum / $n, 1)
        MaxLuma  = $max
        Colors   = $colors.Count
    }
}

# Counts pixels that differ by more than $Threshold between two shots of the same
# rectangle. This is the assertion that a thumbnail was actually composited: the
# control frame is the desktop where the popup will be, so any real change is the
# popup plus its DWM thumbnail.
#
# The locals are deliberately NOT named $a / $b: PowerShell variable names are
# case-insensitive, so $a would BE the [string]$A parameter, and the type
# constraint silently coerces the Bitmap back to its path string.
function Compare-Shots([string]$PathA, [string]$PathB, [int]$Threshold) {
    Add-Type -AssemblyName System.Drawing
    $bmpA = New-Object System.Drawing.Bitmap($PathA)
    $bmpB = New-Object System.Drawing.Bitmap($PathB)
    if ($bmpA.Width -ne $bmpB.Width -or $bmpA.Height -ne $bmpB.Height) {
        $bmpA.Dispose(); $bmpB.Dispose()
        return -1
    }
    $diff = 0; $total = 0
    for ($y = 0; $y -lt $bmpA.Height; $y += 2) {
        for ($x = 0; $x -lt $bmpA.Width; $x += 2) {
            $ca = $bmpA.GetPixel($x, $y); $cb = $bmpB.GetPixel($x, $y)
            $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
            if ($d -gt $Threshold) { $diff++ }
            $total++
        }
    }
    $bmpA.Dispose(); $bmpB.Dispose()
    return [pscustomobject]@{ Changed = $diff; Total = $total; Ratio = [Math]::Round($diff / $total, 4) }
}

# ---------------------------------------------------------------- start the app
# The config is backed up and restored per case: a drop case (T-014) leaves extra
# tiles, and the tile index in "preview shown tile=" is meaningless if the row is
# not the same row the screenshot shows.
Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if ($ConfigBackup -and (Test-Path $ConfigBackup)) {
    Copy-Item $ConfigBackup $configPath -Force
}

# Visual evidence must be solid: the blur tier sets WDA_EXCLUDEFROMCAPTURE, which
# makes CopyFromScreen return whatever is behind the dock (C-16j item 2).
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
$env:DOCKFORGE_LOG_LEVEL = 'debug'
$env:DOCKFORGE_HOVER_PROBE_FILE = $probeFile
if ($PinApps) { $env:DOCKFORGE_PIN_APPS = $PinApps }
else { Remove-Item Env:\DOCKFORGE_PIN_APPS -ErrorAction SilentlyContinue }
if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }

# Start with no hover, so case 1 is not polluted by a leftover pin.
Set-HoverProbe -1
# The log is append-only and shared across runs, so the offset has to be taken
# BEFORE the app starts: reading from 0 finds the previous run's drop-grid line and
# the probe then aims at a row that no longer exists (measured: a 1-tile pinned run
# read the 12-slot line from the run before it and aimed at tile 6 of a 1-tile row).
$runOffset = Get-LogLength
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs

$rect = Get-DockRect
if (-not $rect) { throw 'dock window not found' }
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top

# The tile centres come from the app's own drop-grid log line
#   "drop grid scale=2.000 slots=12 first=69.4 last=685.4 dip"
# which is first_centre + i*pitch in window DIP - the same analytic row
# IdleCentersDip produces. Reading it out of the log means the probe aims where
# the app itself says the tiles are, instead of recomputing the row a second time
# (the same argument that killed the second matching implementation).
$gridLine = $null
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline -and -not $gridLine) {
    $gridLine = Read-LogFrom $runOffset -split "`n" |
        Where-Object { $_ -match '\[dock\] drop grid scale=' } | Select-Object -Last 1
    if (-not $gridLine) { Start-Sleep -Milliseconds 200 }
}
if (-not $gridLine) { throw 'drop grid log line not found; the dock never placed its row' }
$firstDip = [double]($gridLine | Select-String -Pattern 'first=([0-9.]+)' -AllMatches).Matches[0].Groups[1].Value
$lastDip = [double]($gridLine | Select-String -Pattern 'last=([0-9.]+)' -AllMatches).Matches[0].Groups[1].Value
$slotCount = [int]($gridLine | Select-String -Pattern 'slots=(\d+)' -AllMatches).Matches[0].Groups[1].Value
$scaleDock = [double]($gridLine | Select-String -Pattern 'scale=([0-9.]+)' -AllMatches).Matches[0].Groups[1].Value
$pitchDip = if ($slotCount -gt 1) { ($lastDip - $firstDip) / ($slotCount - 1) } else { 0.0 }

$report = New-Object System.Collections.Generic.List[string]
$report.Add("dpi=$Dpi dock=$($rect.Left),$($rect.Top) ${w}x${h} pid=$($proc.Id)")
$report.Add("grid=$gridLine")
$report.Add("slots=$slotCount pitch=$([Math]::Round($pitchDip,2))dip scale=$scaleDock")

# The hovered tile is the one the app reports as the dwell target; the probe aims
# at its centre in DIP, converted with the dock's own scale so a forced DPI
# override cannot desynchronise the two.
$hoveredTile = [int]([Math]::Floor($slotCount / 2))
$centreDip = $firstDip + $hoveredTile * $pitchDip
$centreX = [int]($rect.Left + $centreDip * $scaleDock)
# Mid-plate: the idle region top band is dead space, and a pointer parked there
# never reaches the dock at all (same reason the drop cases aim low). Only used to
# crop the screenshot; the hover itself comes from the probe file.
$plateY = [int]($rect.Bottom - $h * 0.25)
$report.Add("target=($centreX,$plateY) tile=$hoveredTile centre=$([Math]::Round($centreDip,1))dip")
$report.Add("log=$(Get-LatestLog).FullName")
$report.Add("probe_file=$probeFile")

# ------------------------------------------------------- case 1: short dwell
$offset = Get-LogLength
Set-HoverProbe $hoveredTile
Start-Sleep -Milliseconds $ShortDwellMs
# Leave before reading: the latch fires on dwell, so the log must be quiet while
# still hovering, and leaving is what lets a later case re-arm.
Set-HoverProbe -1
Start-Sleep -Milliseconds 700
$tail = Read-LogFrom $offset
$shortHit = @($tail -split "`n" | Where-Object { $_ -match '\[preview\] shown tile=' })
$report.Add('--- case=short_dwell ---')
$report.Add("dwell_ms=$ShortDwellMs shown_lines=$($shortHit.Count)")
foreach ($l in $shortHit) { $report.Add($l) }

# -------------------------------------------------------- case 2: long dwell
# A control frame of the popup's rectangle is taken FIRST, with the popup hidden, so
# the diff measures the popup and its thumbnail rather than "the desktop happens to
# be colourful". The rectangle is derived from the same numbers the app will log:
# slot 110x70 DIP, gap 8, padding 12 (C-17 constants), the drop-grid row, and the
# dock rect. A mismatch against the "shown" line is itself a finding.
#
# The window count is not known until the popup is shown, and the popup is centred on
# the tile, so the capture region is sized for the widest popup one app can produce
# (kMaxThumbnails = 8 windows, C-17c) and centred on the tile. Sizing it from the dock's
# tile count instead under-covers the popup: a pinned run has one tile but that tile's
# app owned three windows, so a 1-slot region captured only the left third of a 740px
# popup. The rect assertion is made later against the count the app actually reports.
$slotW = 110; $slotH = 70; $slotGap = 8; $slotPad = 12; $maxSlotsPerApp = 8
$wideSlots = $maxSlotsPerApp
$wideW = [int][Math]::Round(($slotPad * 2 + $wideSlots * ($slotW + $slotGap) - $slotGap) * $scaleDock)
$predH = [int][Math]::Round(($slotPad * 2 + $slotH) * $scaleDock)
$centrePx = $rect.Left + [int][Math]::Round($centreDip * $scaleDock)
$pad = 24
$shotW = $wideW + $pad * 2
$shotH = $predH + $pad * 2
$shotLeft = [Math]::Max(0, $centrePx - [int]($wideW / 2) - $pad)
$shotTop = [Math]::Max(0, ($rect.Top - $predH - 8) - $pad)
$controlShot = Join-Path $OutDir 'preview_control.png'
Save-Shot $controlShot $shotLeft $shotTop $shotW $shotH
$report.Add("control=$controlShot rect=$shotLeft,$shotTop ${shotW}x${shotH} widest_popup=${wideW}x${predH} slots_cap=$wideSlots")

$offset = Get-LogLength
Set-HoverProbe $hoveredTile
$shown = Wait-LogLine $offset '\[preview\] shown tile=' ($LongDwellMs + 2500)
$report.Add('--- case=long_dwell ---')
$shownTile = -1
$diff = $null
$innerDiff = $null
$minShot = $null
$minDiff = $null
$minStats = $null
$rectMatches = $false
$wanted = 0
$regCount = 0
if ($shown) {
    $report.Add("line=$shown")
    # The hovered tile index must be the one the probe aimed at. A mismatch means
    # the probe and the app disagree about where the row is, and every later
    # assertion would be measuring a different tile than the screenshot shows.
    $shownTile = [int]($shown | Select-String -Pattern 'tile=(\d+)' -AllMatches).Matches[0].Groups[1].Value
    $report.Add("shown_tile=$shownTile aimed_tile=$hoveredTile match=$($shownTile -eq $hoveredTile)")
    # Thumbnail registration is paced at one per tick (C-17c), so a multi-window popup
    # can log "registered=1 pending=2" at the moment it first appears. Measured on a
    # three-window 7-Zip tile the remaining two land within 10ms once the sources are
    # warm, but the shutter must still wait for them rather than assume. The count is
    # taken from the SAME offset as the "shown" line: a fresh offset here starts after
    # the registrations were already written and always reads zero.
    $wanted = 0
    if ($shown -match 'windows=(\d+)') { $wanted = [int]$Matches[1] }
    $regCount = 0
    # Ten windows on one tile is a legal input: the popup must cap at kMaxThumbnails
    # and the probe must wait for the cap, not for the window count.
    $expectedRegs = [Math]::Min($wanted, $maxSlotsPerApp)
    $regDeadline = (Get-Date).AddMilliseconds(6000)
    while ((Get-Date) -lt $regDeadline) {
        $regTail = Read-LogFrom $offset
        $regCount = @($regTail -split "`n" | Where-Object { $_ -match '\[thumb\] registered' }).Count
        if ($regCount -ge $expectedRegs) { break }
        Start-Sleep -Milliseconds 150
    }
    $report.Add("registered_sources=$regCount of expected=$expectedRegs windows=$wanted")
    # Shutter strictly after the log line: a fixed sleep here leads the event by
    # a variable amount and produces a capture with nothing in it (T-014 lesson).
    Start-Sleep -Milliseconds 600
    $shot = Join-Path $OutDir 'preview_visible.png'
    # Crop around the POPUP, not around the dock. The "shown" line carries the
    # popup's own rectangle, and that is the only region the assertion is about;
    # centring on the dock instead produced a 107px-wide strip for a 1-tile row
    # while the popup sat at 1305,1399 - entirely outside the capture.
    # Now that the app has reported how many windows the tile owns, the popup geometry
    # is predictable: width from the slot count, height from one slot, and the rect is
    # centred on the tile and stacked 8px above the dock.
    $popupW = 320; $popupH = 240; $popupLeft = 0; $popupTop = 0
    if ($shown -match 'rect=(-?\d+),(-?\d+) (\d+)x(\d+)') {
        $popupLeft = [int]$Matches[1]; $popupTop = [int]$Matches[2]
        $popupW = [int]$Matches[3]; $popupH = [int]$Matches[4]
    }
    $shownSlots = [Math]::Min($wanted, $maxSlotsPerApp)
    if ($shownSlots -lt 1) { $shownSlots = 1 }
    $predW = [int][Math]::Round(($slotPad * 2 + $shownSlots * ($slotW + $slotGap) - $slotGap) * $scaleDock)
    $predLeft = $centrePx - [int]($predW / 2)
    $predTop = $rect.Top - $predH - 8
    $rectMatches = ($popupLeft -eq $predLeft) -and ($popupTop -eq $predTop) -and ($popupW -eq $predW) -and ($popupH -eq $predH)
    $report.Add("popup_actual=$popupLeft,$popupTop ${popupW}x${popupH} predicted=$predLeft,$predTop ${predW}x${predH} matches_prediction=$rectMatches")
    # Pad generously: the popup rect is the client area, and the plate border plus
    # the rounded corners fall outside it.
    Save-Shot $shot $shotLeft $shotTop $shotW $shotH
    $report.Add("shot=$shot rect=$shotLeft,$shotTop ${shotW}x${shotH}")
    $diff = Compare-Shots $controlShot $shot 24
    $report.Add("diff_vs_control changed=$($diff.Changed)/$($diff.Total) ratio=$($diff.Ratio)")
    # The capture region is sized for the widest popup one app can own (eight windows),
    # so on a three-window tile most of it is untouched desktop. Diluting the assertion
    # over that empty area makes the ratio depend on how many windows the app happened
    # to open rather than on whether DWM composited anything. Re-crop both frames to the
    # popup's own rectangle and diff those: that is the region the claim is about.
    $padInShot = 24
    $innerW = [Math]::Min($popupW, $shotW - 2 * $padInShot)
    $innerH = [Math]::Min($popupH, $shotH - 2 * $padInShot)
    $innerLeft = [Math]::Max(0, $popupLeft - $shotLeft)
    $innerTop = [Math]::Max(0, $popupTop - $shotTop)
    $cropA = Join-Path $OutDir 'preview_control_popup.png'
    $cropB = Join-Path $OutDir 'preview_visible_popup.png'
    Save-Crop $controlShot $cropA $innerLeft $innerTop $innerW $innerH
    Save-Crop $shot $cropB $innerLeft $innerTop $innerW $innerH
    $innerDiff = Compare-Shots $cropA $cropB 24
    $report.Add("diff_popup_only changed=$($innerDiff.Changed)/$($innerDiff.Total) ratio=$($innerDiff.Ratio) rect=$innerLeft,$innerTop ${innerW}x${innerH}")
    $stats = Get-ShotStats $shot
    $report.Add("shot_stats colors=$($stats.Colors) mean_luma=$($stats.MeanLuma) max_luma=$($stats.MaxLuma)")

    # Per-source lines prove C-17g: for a packaged app the frame host hwnd and the
    # real app_pid must differ, and the source must be the frame host.
    $srcLines = @((Read-LogFrom $offset) -split "`n" | Where-Object { $_ -match '\[preview\] source\[' })
    $report.Add("source_lines=$($srcLines.Count)")
    foreach ($l in $srcLines) { $report.Add($l) }
} else {
    $report.Add('line=NONE (verdict FAIL: dwell exceeded the latch without a popup)')
}

# ------------------------------------------- case 3: leave, then re-arm once
$offset = Get-LogLength
Set-HoverProbe -1
$hidden = Wait-LogLine $offset '\[preview\] hidden' 2000
$report.Add('--- case=leave ---')
$report.Add("hidden_line=$(if ($hidden) { $hidden } else { 'NONE' })")

$offset = Get-LogLength
Set-HoverProbe $hoveredTile
$again = Wait-LogLine $offset '\[preview\] shown tile=' ($LongDwellMs + 2500)
$report.Add('--- case=rearm ---')
$report.Add("second_line=$(if ($again) { $again } else { 'NONE (verdict FAIL: dwell did not re-arm)' })")

Set-HoverProbe -1
Start-Sleep -Milliseconds 400

# ------------------------------------- case 4: minimized source keeps its last frame
# DWM retains the last composed frame of a minimized window (evidence/T-015/bench.txt
# section C measured best_delta=0 against the pre-minimize colours), so a slot whose
# source was minimized must still show the window's contents. The failure this rules
# out is a popup that goes black because registration raced the minimize.
#
# The comparison is against the frame captured in case 2, over the popup's own
# rectangle: same position, same size, same sources - only the window state changed.
if ($MinimizeSource -and $shown -and $innerDiff) {
    $report.Add('--- case=minimized_source ---')
    # Minimize EVERY top-level window of the matching processes, not just
    # MainWindowHandle: an app with four windows reports one main window, and the
    # slots for the other three would keep their visible frames and quietly weaken
    # the assertion. EnumWindows + GetWindowThreadProcessId is the same rule the app
    # itself uses to find windows.
    $minPids = @()
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue)) {
        if ($MinimizeProcess -and ($p.ProcessName -notlike "*$MinimizeProcess*")) { continue }
        if ($p.MainWindowHandle -eq 0) { continue }
        $minPids += $p.Id
    }
    $minHandles = @(Get-TopLevelWindows -Pids $minPids)
    foreach ($h in $minHandles) { [void][T016Win]::ShowWindow($h, 6) }   # SW_MINIMIZE
    $report.Add("minimized_pids=$($minPids -join ',') count=$($minPids.Count) windows=$($minHandles.Count)")
    Start-Sleep -Milliseconds 900

    $offset = Get-LogLength
    Set-HoverProbe $hoveredTile
    $minShown = Wait-LogLine $offset '\[preview\] shown tile=' ($LongDwellMs + 2500)
    $report.Add("min_line=$(if ($minShown) { $minShown } else { 'NONE' })")
    if ($minShown) {
        $minRegs = 0
        $minDeadline = (Get-Date).AddMilliseconds(6000)
        while ((Get-Date) -lt $minDeadline) {
            $minRegs = @((Read-LogFrom $offset) -split "`n" | Where-Object { $_ -match '\[thumb\] registered' }).Count
            if ($minRegs -ge $expectedRegs) { break }
            Start-Sleep -Milliseconds 150
        }
        Start-Sleep -Milliseconds 600
        $minShot = Join-Path $OutDir 'preview_minimized.png'
        Save-Shot $minShot $shotLeft $shotTop $shotW $shotH
        $minCrop = Join-Path $OutDir 'preview_minimized_popup.png'
        Save-Crop $minShot $minCrop $innerLeft $innerTop $innerW $innerH
        # Same-rectangle comparison, so a high ratio means "the slot still shows the
        # window"; a black slot would differ from the visible frame almost everywhere.
        $minDiff = Compare-Shots $cropB $minCrop 24
        $minStats = Get-ShotStats $minCrop
        $report.Add("min_registered=$minRegs of expected=$expectedRegs")
        $report.Add("min_vs_visible changed=$($minDiff.Changed)/$($minDiff.Total) ratio=$($minDiff.Ratio)")
        $report.Add("min_shot_stats colors=$($minStats.Colors) mean_luma=$($minStats.MeanLuma) max_luma=$($minStats.MaxLuma)")
        $report.Add("min_shot=$minShot")
    }
    Set-HoverProbe -1
    Start-Sleep -Milliseconds 400
    # Restore what this case minimized, through the same handle list, so the desktop is
    # left as it was found.
    foreach ($h in $minHandles) { [void][T016Win]::ShowWindow($h, 9) }   # SW_RESTORE
    $report.Add("restored_windows=$($minHandles.Count)")}

# ------------------------------------------------------------------- verdict
$verdict = 'PASS'
if ($shortHit.Count -gt 0) { $verdict = 'FAIL' }
if (-not $shown) { $verdict = 'FAIL' }
if (-not $again) { $verdict = 'FAIL' }
if ($shown -and ($shownTile -ne $hoveredTile)) { $verdict = 'FAIL' }
if (-not $hidden) { $verdict = 'FAIL' }
# The popup must actually change pixels. A "shown" log line proves the code ran; it
# does not prove DWM composited anything, and a thumbnail that failed to register
# leaves the popup background-only. The control frame is the same rectangle with the
# popup hidden, so any real change is the popup plus its thumbnail. The popup-only
# crop is the assertion; the wide-frame ratio is reported for context only.
if ($innerDiff -and ($innerDiff.Ratio -lt 0.10)) { $verdict = 'FAIL' }
# The popup rectangle is fully determined by the drop grid, the dock rect and the
# window count. A mismatch means the placement math and the constants disagree, and
# the diff above was taken over a region the popup does not occupy.
if ($shown -and (-not $rectMatches)) { $verdict = 'FAIL' }
# Every source the app promised must have been registered; otherwise the pixels
# prove only the first slot and C-17c's pacing was never exercised. The promise is
# capped at kMaxThumbnails: ten windows on one tile is a legal input and must show
# eight slots, so comparing against the raw window count would fail a correct run.
$expectedRegs = [Math]::Min($wanted, $maxSlotsPerApp)
if ($expectedRegs -gt 1 -and $regCount -lt $expectedRegs) { $verdict = 'FAIL' }
# A minimized source must keep its last composed frame (C-17i). If DWM had dropped it,
# the slot would be the popup background alone: every pixel inside the slot would flip,
# so the same-rectangle ratio against the visible frame would approach 1. Measured on a
# three-window 7-Zip tile the ratio was 0.0143 - the frame is intact.
if ($MinimizeSource -and $shown) {
    if (-not $minShot) { $verdict = 'FAIL' }
    elseif ($minDiff.Ratio -gt 0.50) { $verdict = 'FAIL' }
    elseif ($minStats.Colors -lt 32) { $verdict = 'FAIL' }
}
$report.Add('--- verdict ---')
$report.Add("verdict=$verdict")
$report.Add('note=short_dwell must have zero shown_lines; long_dwell and rearm must each have one; shown_tile must equal aimed_tile; leave must emit a hidden line; popup must differ from the control frame by >10% of sampled pixels')

$outPath = Join-Path $OutDir 'preview_dwell.txt'
[System.IO.File]::WriteAllText($outPath, ($report -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
$report | ForEach-Object { Write-Output $_ }
