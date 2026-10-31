# DockForge T-301 evidence probe: the Finder Bar, live.
#
# Same shutter pattern as every other probe in this repo: PASS/FAIL comes from the
# app's own log lines plus its state-file JSON, never from the eye. Screenshots
# document the visuals; they are not the evidence.
#
# Chain under test (T-301 DoD + C-31 + C-32):
#   A  the bar exists as a real window at the top of the primary display, 28 dip tall
#      (56px at 192 dpi), full width, and is retracted to a hairline by default.
#   B  auto-hide: the pointer reaching the top edge expands it; leaving retracts it.
#      A retracted bar must not be clickable - its window region is empty.
#   C  an application with no Win32 menu (the normal case: spike Q3 measured 11 visible
#      windows, 2 with menus, 0 of them explorer or Chrome) shows glyph + name only.
#   D  an application WITH a menu: the bar reads the foreign HMENU and lays out its
#      top-level titles (C-32 "read, never take over").
#   E  selecting an item forwards WM_COMMAND to the owning application, and the
#      application really executes it.
#   F  the system menu opens on its own thread, the bar stays alive while it is up,
#      and the popup can always be dismissed (WM_CANCELMODE escape, spike Q13).
#   G  screenshots: bar_retracted.png, bar_expanded.png, menu_open.png.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\finderbar_probe.ps1
param(
    [string]$OutDir = 'evidence\T-301',
    [string]$Exe = 'build\bin\DockForge.exe'
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$script:failures = New-Object 'System.Collections.Generic.List[string]'

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    $script:failures.Add($why)
}
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  PASS $what" } else { Fail $what }
}
function Stop-All {
    foreach ($n in @('DockForge', 'menu_target')) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

Stop-All
Start-Sleep -Milliseconds 400
if (-not (Test-Path $Exe)) { Write-Host "FAIL: app missing: $Exe"; exit 1 }
if (-not (Test-Path 'spikes\T-301_finderbar_menu\menu_target.exe')) {
    Write-Host 'FAIL: menu_target.exe missing (the T-301 menu fixture)'
    exit 1
}

$scratch = Join-Path $env:TEMP ('dockforge-t301-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch 'config.json'
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
# UTF-8 without BOM: the config reader rejects a BOM, and Set-Content -Encoding UTF8
# writes one on PowerShell 5.1.
[System.IO.File]::WriteAllText($configPath, '{}', (New-Object System.Text.UTF8Encoding($false)))

$cmdFile = Join-Path $scratch 'bar-cmd.txt'
$stateFile = Join-Path $scratch 'bar-state.json'
$targetState = Join-Path $scratch 'target-state.txt'
New-Item -ItemType File -Force -Path $cmdFile | Out-Null

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_DUMP_DIR = $scratch
$env:DOCKFORGE_FINDERBAR = '1'
$env:DOCKFORGE_FINDERBAR_CMD_FILE = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
# The blur tier cannot be captured on this host (a known DComp/capture limitation,
# see STATE.json known_gotchas); solid proves the geometry and the text instead.
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T301Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public static IntPtr Find(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    // Same as Find but without the visibility filter: the menu owner is a 1x1 popup that
    // is never shown, so a visible-only search can never see it.
    public static IntPtr FindAny(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr At(int x, int y) {
        POINT p; p.X = x; p.Y = y;
        return WindowFromPoint(p);
    }
}
'@
Add-Type -AssemblyName System.Drawing
[T301Probe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# The app samples the command file every 50ms and ignores a repeat of the same
# content, so every command carries a unique token.
function Send-Cmd([string]$text, [int]$settleMs = 300) {
    if (Test-Path $stateFile) { Remove-Item $stateFile -Force }
    $body = "$text`n#" + [guid]::NewGuid().ToString('N')
    [System.IO.File]::WriteAllText($cmdFile, $body, (New-Object System.Text.UTF8Encoding($false)))
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path $stateFile) { break }
    }
    Start-Sleep -Milliseconds $settleMs
}

function Get-State {
    if (-not (Test-Path $stateFile)) { return $null }
    try { Get-Content $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $null }
}

function Save-Shot([string]$name, [int]$x, [int]$y, [int]$w, [int]$h) {
    if ($w -le 0 -or $h -le 0) { Write-Host "  (empty rect for $name)"; return }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("  shot {0} {1}x{2} at {3},{4}" -f $name, $w, $h, $x, $y)
}

# Counts pixels in a box that differ from the plate colour. The state JSON can say a label
# was laid out; only pixels can say it was drawn. Single-point sampling is useless for 13dip
# text (a probe pixel lands between two strokes as often as on one), so this counts ink over
# the whole box instead.
#
# The reference colour comes from an explicit background point, never from inside the
# measured box: the bottom row of a 56px strip is already desktop, so using it as "the plate
# colour" marks the entire bar as ink and every measurement becomes meaningless.
function Measure-Ink([string]$png, [int]$left, [int]$top, [int]$right, [int]$bottom,
                     [int]$bgX = 2000, [int]$bgY = 28) {
    $bmp = [System.Drawing.Bitmap]::FromFile((Resolve-Path (Join-Path $OutDir $png)))
    $ink = 0
    $total = 0
    $bg = $bmp.GetPixel($bgX, $bgY)
    for ($x = [int]$left; $x -lt [int]$right; $x++) {
        for ($y = [int]$top; $y -lt [int]$bottom; $y++) {
            $c = $bmp.GetPixel($x, $y)
            $total++
            if ([math]::Abs($c.R - $bg.R) -gt 24 -or [math]::Abs($c.G - $bg.G) -gt 24 -or
                [math]::Abs($c.B - $bg.B) -gt 24) { $ink++ }
        }
    }
    $bmp.Dispose()
    if ($total -eq 0) { return 0.0 }
    return [math]::Round(100.0 * $ink / $total, 2)
}

Write-Host "=== T-301 probe: scratch=$scratch ==="
Start-Process -FilePath (Join-Path $root $Exe) | Out-Null
Start-Sleep -Seconds 3

$log = Latest-Log
if (-not $log) { Write-Host 'FAIL: no log file written'; Stop-All; exit 1 }
$text = Get-Content $log.FullName -Raw -Encoding UTF8
if ($text -notmatch 'Finder Bar ready') { Write-Host 'FAIL: bar did not come up'; Stop-All; exit 1 }

# ---- A: geometry ---------------------------------------------------------------
Write-Host '--- A: bar window geometry'
$barHwnd = [T301Probe]::Find('DockForgeFinderBar')
Check ($barHwnd -ne [IntPtr]::Zero) 'bar window exists (class DockForgeFinderBar)'
$barRect = New-Object T301Probe+RECT
[T301Probe]::GetWindowRect($barHwnd, [ref]$barRect) | Out-Null
$barW = $barRect.Right - $barRect.Left
$barH = $barRect.Bottom - $barRect.Top
Write-Host ("  bar rect = {0},{1},{2},{3} ({4}x{5})" -f $barRect.Left, $barRect.Top, $barRect.Right, $barRect.Bottom, $barW, $barH)
Check ($barRect.Left -eq 0 -and $barRect.Top -eq 0) 'bar sits at the top-left of the primary display'
Check ($barH -eq 56) 'bar is 56px tall (28 dip at 192 dpi, C-31)'

Send-Cmd 'state'
$s = Get-State
if ($s -eq $null) { Write-Host 'FAIL: no state snapshot'; Stop-All; exit 1 }
Write-Host ("  width_dip={0} height_dip={1} dpi={2}" -f $s.width_dip, $s.height_dip, $s.dpi)
Check ($s.height_dip -eq 28) 'height is 28 dip'
Check ($s.dpi -eq 192) 'dpi is 192'
Check ([math]::Abs($s.width_dip - ($barW / 2.0)) -lt 1.0) 'width_dip matches the window width in dip'

# ---- B: auto-hide --------------------------------------------------------------
Write-Host '--- B: auto-hide'
Check ([bool]$s.auto_hide) 'auto_hide is on by default'
# Park the pointer well away from the top edge first: the bar's state depends on where
# the physical cursor is, and a probe must not inherit whatever the previous run left.
[T301Probe]::SetCursorPos([int]($barW / 2), 900) | Out-Null
Start-Sleep -Milliseconds 700
Send-Cmd 'state'
$s = Get-State
Write-Host ("  retracted: progress={0} offset={1} interactive={2}" -f $s.progress, $s.offset_dip, $s.interactive)
Check ([math]::Abs($s.offset_dip - (-24.0)) -lt 0.01) 'retracted bar is offset by -(28-4) dip'
Check ([bool]$s.interactive -eq $false) 'retracted bar is not interactive'
$hit = [T301Probe]::At([int]($barW / 2), 28)
Check ($hit -ne $barHwnd) 'retracted bar does not capture clicks (empty window region)'
Save-Shot 'bar_retracted.png' 0 0 $barW 56

# Pointer to the very top edge: the trigger band is 3 dip tall.
[T301Probe]::SetCursorPos([int]($barW / 2), 1) | Out-Null
Start-Sleep -Milliseconds 700
Send-Cmd 'state'
$s = Get-State
Write-Host ("  after pointer at top: progress={0} offset={1} interactive={2}" -f $s.progress, $s.offset_dip, $s.interactive)
Check ([double]$s.progress -ge 0.99) 'pointer at the top edge fully expands the bar'
Check ([math]::Abs($s.offset_dip) -lt 0.01) 'expanded bar has zero offset'
Check ([bool]$s.interactive) 'expanded bar is interactive'
$hit = [T301Probe]::At([int]($barW / 2), 28)
Check ($hit -eq $barHwnd) 'expanded bar captures clicks'
Save-Shot 'bar_expanded.png' 0 0 $barW 56

# The bar must be *visible* - a DComp surface with WDA_EXCLUDEFROMCAPTURE renders nothing
# in any screenshot, which would make every visual claim below unverifiable. Ink over the
# left group proves text was actually drawn where the layout said it would be.
#
# Both measurements stay inside rows 4..52. The plate's own border stroke and its rounded
# corners live in the first and last few rows, so including them counts the frame itself as
# content and makes an empty stretch look 7% inked.
$inkLeft = Measure-Ink 'bar_expanded.png' 20 4 380 52
$inkEmpty = Measure-Ink 'bar_expanded.png' 900 4 1400 52
Write-Host ("  ink: left group={0}%  empty stretch={1}%" -f $inkLeft, $inkEmpty)
Check ($inkLeft -gt 3.0) 'the left group has drawn content in the screenshot'
Check ($inkEmpty -lt 0.5) 'the empty stretch of the bar is a flat plate'

# ---- C/D/E: a real application's menu -------------------------------------------
Write-Host '--- C/D/E: reading and forwarding a foreign menu'
$tgt = Start-Process -FilePath 'spikes\T-301_finderbar_menu\menu_target.exe' -ArgumentList $targetState -PassThru
for ($i = 0; $i -lt 50; $i++) {
    Start-Sleep -Milliseconds 100
    if (Test-Path $targetState) { break }
}
Start-Sleep -Milliseconds 500
$readyLine = (Get-Content $targetState -Encoding ASCII | Select-String '^ready ' | Select-Object -First 1)
if (-not $readyLine) { Write-Host 'FAIL: fixture never reported its window'; Stop-All; exit 1 }
$targetHwnd = ($readyLine.ToString() -replace '^ready ', '').Trim()
Write-Host "  fixture hwnd = $targetHwnd"

# Attach pins the tracked application: a probe process cannot win a foreground race
# against a terminal (measured). Only the input source changes - naming, menu reading,
# layout, painting, the menu thread and the forwarding are all the production path.
Send-Cmd "attach $targetHwnd"
Send-Cmd 'state'
$s = Get-State
Write-Host ("  foreground_name={0} menu_readable={1} titles=[{2}]" -f $s.foreground_name, $s.menu_readable, ($s.menu_titles -join ','))
Check ($s.foreground_name -eq 'menu_target') 'the tracked application is the fixture'
Check ([bool]$s.menu_readable) 'the bar read the foreign menu'
Check (($s.menu_titles -join ',') -eq 'File,Edit,View,Help') 'top-level titles are File,Edit,View,Help'
Check ($s.layout.placed_menus -eq 4) 'all four titles were placed'
$menuBoxes = @($s.boxes | Where-Object { $_.menu_index -ge 0 })
Check ($menuBoxes.Count -eq 4) 'four menu boxes carry a menu_index'
Check ($menuBoxes[0].label -eq 'File' -and $menuBoxes[3].label -eq 'Help') 'menu boxes are ordered left to right'
$nameBox = @($s.boxes | Where-Object { $_.name })[0]
Check ($nameBox.label -eq 'menu_target') 'the application name comes from the executable, not the title'

# The state file proves the layout; the screenshot proves the paint. Box coordinates are
# DIP and the capture is physical pixels, so every box has to be scaled by dpi/96 before
# it can index into the image.
$pxPerDip = $s.dpi / 96.0
Save-Shot 'bar_with_menu.png' 0 0 $barW 56
foreach ($b in $menuBoxes) {
    $l = [int]([double]$b.left * $pxPerDip); $r = [int]([double]$b.right * $pxPerDip)
    $ink = Measure-Ink 'bar_with_menu.png' $l 4 $r 52
    Write-Host ("  ink '{0}' px[{1}..{2}] = {3}%" -f $b.label, $l, $r, $ink)
    Check ($ink -gt 2.0) ("menu title '{0}' is drawn on the bar" -f $b.label)
}
$nl = [int]([double]$nameBox.left * $pxPerDip); $nr = [int]([double]$nameBox.right * $pxPerDip)
$inkName = Measure-Ink 'bar_with_menu.png' $nl 4 $nr 52
Write-Host ("  ink app name px[{0}..{1}] = {2}%" -f $nl, $nr, $inkName)
Check ($inkName -gt 2.0) 'the application name is drawn on the bar'

# Box 2 is File (0 = glyph, 1 = name); item 1 inside File is Open, id 4002.
Send-Cmd 'pick 2 1'
Start-Sleep -Milliseconds 500
$targetText = Get-Content $targetState -Encoding ASCII -Raw
Write-Host ("  fixture state = {0}" -f (($targetText -split "`n" | Where-Object { $_ }) -join ' | '))
Check ($targetText -match 'command 4002') 'selecting File>Open forwarded WM_COMMAND 4002 and the app ran it'

# ---- F: menu open / cancel ------------------------------------------------------
Write-Host '--- F: system menu lifecycle'
Send-Cmd 'menu 0'
$s = Get-State
Check ([bool]$s.menu_open) 'the system menu opened'
Check ($s.menu_opens -ge 1) 'menu_opens counted the popup'
$menuOwner = [IntPtr]::Zero
# The owner window is created on the menu thread, so it does not exist the instant the
# command returns, and it is a hidden 1x1 popup by design - the visible-only search can
# never find it. Poll a class search that ignores visibility.
for ($i = 0; $i -lt 30; $i++) {
    $menuOwner = [T301Probe]::FindAny('DockForgeFinderBarMenuOwner')
    if ($menuOwner -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 50
}
Check ($menuOwner -ne [IntPtr]::Zero) 'the popup runs on its own owner window'
Save-Shot 'menu_open.png' 0 0 420 380
# The bar must still be alive and painting while the popup is modal on its own thread.
$beforePaints = $s.paints
Start-Sleep -Milliseconds 600
Send-Cmd 'state'
$s = Get-State
Check ([bool]$s.menu_open) 'the menu stays open until dismissed'
Send-Cmd 'cancel' 600
Send-Cmd 'state'
$s = Get-State
Write-Host ("  after cancel: menu_open={0} paints={1}" -f $s.menu_open, $s.paints)
Check ([bool]$s.menu_open -eq $false) 'WM_CANCELMODE dismisses the popup'
Check ([int]$s.paints -ge [int]$beforePaints) 'the bar kept painting across the popup'

# ---- G: retraction after the pointer leaves -------------------------------------
Write-Host '--- G: retract again'
[T301Probe]::SetCursorPos([int]($barW / 2), 900) | Out-Null
Start-Sleep -Milliseconds 700
Send-Cmd 'state'
$s = Get-State
Write-Host ("  after pointer leaves: progress={0} interactive={1}" -f $s.progress, $s.interactive)
Check ([double]$s.progress -le 0.01) 'the bar retracts once the pointer leaves'
Check ([bool]$s.interactive -eq $false) 'the retracted bar stops being interactive'

# ---- verdict --------------------------------------------------------------------
Get-Process -Name menu_target -ErrorAction SilentlyContinue | Stop-Process -Force
$logText = Get-Content $log.FullName -Raw -Encoding UTF8
$forwarded = ([regex]::Matches($logText, 'forwarded WM_COMMAND 4002')).Count
Check ($forwarded -ge 1) 'the log records the forwarded command'

if ($script:failures.Count -gt 0) {
    Write-Host ''
    Write-Host ("verdict: FAIL ({0})" -f $script:failures.Count)
    foreach ($f in $script:failures) { Write-Host "  - $f" }
    Stop-All
    exit 1
}
Write-Host ''
Write-Host 'verdict: Q19_bar_geometry=PASS Q20_auto_hide=PASS Q21_foreign_menu=PASS Q22_forwarding=PASS Q23_menu_lifecycle=PASS'
Stop-All
exit 0
