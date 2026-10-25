# DockForge T-066 evidence probe: the folder drill-through panel, live.
#
# Log-driven shutter pattern (same as the other probes): PASS/FAIL is decided
# from the app's own log lines plus saved screenshots - never from the eye.
#
# Chain under test (ADR-0023), all for real:
#   DOCKFORGE_FOLDER_TILE seeds a folder tile through the PRODUCTION drop
#   resolver (ResolveDroppedPath -> CommitOrder) -> `tile <n>` runs ActivateTile
#   (the WM_LBUTTONUP release arm's function) -> folder interception opens the
#   panel instead of Explorer -> rows enumerated via IShellFolder ->
#   drill/back through the nav stack -> watcher refresh (ReadDirectoryChangesW
#   + 500ms debounce) -> Esc unwind (the WM_KEYDOWN function) -> REAL
#   click-outside close (SendInput, the click_probe.ps1 precedent) ->
#   denied/empty placeholders.
#
# Notes:
#  - DOCKFORGE_PIN_APPS with a never-matching substring empties the scan row so
#    the seeded folder tile is the only tile (index parse from the log is kept
#    anyway as belt and braces).
#  - DOCKFORGE_FORCE_MATERIAL=solid pins the plate to the solid tier (the blur
#    backdrop on this machine composites empty; pre-existing, T-065 note).
param(
    [string]$OutDir = 'evidence\T-066',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$SettleMs = 700
)
$ErrorActionPreference = 'Stop'
# Per-monitor DPI awareness MUST be set before any WinForms/Drawing type
# initializes: loading System.Windows.Forms first pins the process to a
# virtualized DPI context and every SetCursorPos/GetWindowRect then lands on
# the wrong monitor (measured: asked (1439,1270), got (808,904)).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class T066Dpi {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    public static void Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
}
'@
[T066Dpi]::Enable()
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

# ---- scratch tree -----------------------------------------------------------
$scratch = Join-Path $env:TEMP ('dockforge-t066-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$sub = Join-Path $scratch 'sub'
New-Item -ItemType Directory -Force -Path $sub | Out-Null
$empty = Join-Path $scratch 'empty'
New-Item -ItemType Directory -Force -Path $empty | Out-Null
Set-Content -Path (Join-Path $scratch 'alpha.txt') -Value 'alpha' -Encoding ASCII
Set-Content -Path (Join-Path $sub 'inner.txt') -Value 'inner' -Encoding ASCII
$longName = 'a-very-long-file-name-that-must-truncate-in-the-row-and-show-in-full-on-hover.txt'
Set-Content -Path (Join-Path $scratch $longName) -Value 'long' -Encoding ASCII

$scratch = (Get-Item $scratch).FullName
$sub = (Get-Item $sub).FullName
$empty = (Get-Item $empty).FullName

# ---- process scaffolding ----------------------------------------------------
$scratchProc = Join-Path $env:TEMP ('dockforge-t066-proc-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratchProc | Out-Null
$configPath = Join-Path $scratchProc 'config.json'
$logDir = Join-Path $scratchProc 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
'{}' | Set-Content -Path $configPath -Encoding ASCII   # no BOM: nlohmann rejects BOMs

$cmdFile = Join-Path $scratchProc 'folder_cmd.txt'
'' | Set-Content -Path $cmdFile -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
$env:DOCKFORGE_FOLDER_TILE = $scratch
$env:DOCKFORGE_FOLDER_CMD_FILE = $cmdFile
$env:DOCKFORGE_PIN_APPS = 'zz_no_such_entry_t066'   # isolate the folder tile

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
function Send-Cmd([string]$cmd) {
    # One command line per write; the app dedups against the previous text, so
    # different consecutive commands always fire. Retried: the app re-reads the
    # file every 50ms and holds an open handle briefly, and a single Set-Content
    # can lose that race ("stream was not readable", measured mid-run).
    for ($try = 1; $try -le 5; ++$try) {
        try {
            Set-Content -Path $cmdFile -Value $cmd -Encoding ASCII -ErrorAction Stop
            return
        } catch {
            Start-Sleep -Milliseconds 80
        }
    }
    Write-Host "  WARN: command write kept failing: $cmd"
}
function Wait-LogLine([string]$pattern, [int]$timeoutMs) {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($timeoutMs)
    while ([DateTime]::UtcNow -lt $deadline) {
        $log = Latest-Log
        if ($log) {
            $m = Select-String -Path $log.FullName -Pattern $pattern -ErrorAction SilentlyContinue
            if ($m) { return $m }
        }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

# ---- window helpers ---------------------------------------------------------
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T066Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, uint data, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    public static IntPtr Find(string cls, bool requireVisible) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls && (!requireVisible || IsWindowVisible(h))) {
                found = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
[T066Probe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Save-PanelShot([string]$name) {
    $h = [T066Probe]::Find('DockForgeFolderPanel', $true)
    if ($h -eq [IntPtr]::Zero) { Write-Host "  (no visible panel to shoot for $name)"; return $false }
    $r = New-Object T066Probe+RECT
    [T066Probe]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($w -le 0 -or $ht -le 0) { Write-Host "  (degenerate panel rect for $name)"; return $false }
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("  shot {0} rect=({1},{2})-({3},{4})" -f $name, $r.Left, $r.Top, $r.Right, $r.Bottom)
    return $true
}

function Click-At([int]$px, [int]$py) {
    # P/Invoke SetCursorPos (physical virtual-screen pixels), NOT .NET
    # Cursor.Position: its coordinates are DPI-virtualized, and on this
    # mixed-DPI multi-monitor machine clicks landed far from the target
    # (measured: asked (4,350), got (602,621)).
    [T066Probe]::SetCursorPos($px, $py) | Out-Null
    Start-Sleep -Milliseconds 150
    [T066Probe]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) | Out-Null  # LEFTDOWN
    Start-Sleep -Milliseconds 60
    [T066Probe]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) | Out-Null  # LEFTUP
    Start-Sleep -Milliseconds 400
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$report = New-Object System.Collections.Generic.List[string]
$report.Add("T-066 probe $(Get-Date -Format s)  scratch=$scratch")

Write-Host "=== T-066 probe: scratch=$scratch ==="

# ---- start the app ----------------------------------------------------------
# Remember the foreground window BEFORE DockForge starts: part H clicks it as
# the "real click outside" - a genuine user-window click, guaranteed to take
# activation (a bare desktop point can sit under a NOACTIVATE tool window,
# which changes nothing - measured on this machine).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class T066Fg {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
}
'@
$preForeground = [T066Fg]::GetForegroundWindow()
if ($preForeground -eq [IntPtr]::Zero) { Fail 'no foreground window before start' }

Start-Process -FilePath (Join-Path $root $Exe)
Start-Sleep -Seconds 4
$log = Latest-Log
if (-not $log) { Fail 'no log file written' }
$text = Get-Content $log.FullName -Raw

# ---- Part A: folder tile seeded through the production drop resolver --------
$seedMatch = Select-String -Path $log.FullName -Pattern 'seeded a folder tile at \[(\d+)\]' |
    Select-Object -First 1
if (-not $seedMatch) {
    Fail "folder tile not seeded: $((Get-Content $log.FullName | Select-String 'FOLDER_TILE') -join '; ')"
}
$tileIndex = [int]$seedMatch.Matches[0].Groups[1].Value
Write-Host "A  PASS  folder tile seeded at [$tileIndex] (production drop path)"
$report.Add("A  PASS  folder tile seeded at [$tileIndex] (production drop path)")

# ---- Part B: a REAL click on the folder tile opens the panel ----------------
# The dock click is the production gesture: it grants DockForge the foreground
# right, the panel's SetForegroundWindow succeeds, and the keyboard therefore
# works from the first keystroke. A `tile` command from the tick cannot grant
# that right (measured: the panel then stays inactive and Esc has no target).
$dock = [T066Probe]::Find('DockForgeHostWindow', $true)
if ($dock -eq [IntPtr]::Zero) { Fail 'dock window not found' }
$dk = New-Object T066Probe+RECT
[T066Probe]::GetWindowRect($dock, [ref]$dk) | Out-Null
$opened = $false
foreach ($attempt in 1..3) {
    Click-At ([int](($dk.Left + $dk.Right) / 2)) ([int]($dk.Bottom - 35))
    if (Wait-LogLine ("folder panel opened for tile\[$tileIndex\]") 2500) { $opened = $true; break }
    # The injected click occasionally lands elsewhere on this host (the DPI
    # context of a PowerShell process is not reliably settable); a retry is
    # cheaper than a false FAIL.
    Start-Sleep -Milliseconds 300
}
if (-not $opened) { Fail 'dock tile click did not open the folder panel' }
Start-Sleep -Milliseconds $SettleMs
if ([T066Probe]::Find('DockForgeFolderPanel', $true) -eq [IntPtr]::Zero) {
    Fail 'panel window not visible after open'
}
if (-not (Save-PanelShot 'panel_open.png')) { Fail 'panel screenshot failed' }
$after = Get-Content (Latest-Log).FullName -Raw
if ($after -match 'folder panel open failed') { Fail 'panel open failed inside the app' }
Write-Host 'B  PASS  REAL dock tile click opened the panel (not Explorer)'
$report.Add('B  PASS  REAL dock tile click opened the panel (not Explorer)')

# ---- Part C: hover shows the full name in the hint strip --------------------
# Rows are folder-first, then case-insensitive names:
#   [0]=empty, [1]=sub, [2]=<long name>, [3]=alpha.txt
Send-Cmd 'hover 2'
if (-not (Wait-LogLine 'probe hover row=2' 3000)) { Fail 'hover command did not register' }
Start-Sleep -Milliseconds $SettleMs
Save-PanelShot 'panel_hover_fullname.png' | Out-Null
Write-Host 'C  PASS  hover state set on the long-name row; hint strip in shot'
$report.Add('C  PASS  hover state set on the long-name row; hint strip in shot')

# ---- Part D: drill into the subfolder ---------------------------------------
Send-Cmd 'drill sub'
if (-not (Wait-LogLine 'reload\(drill\) status=ok rows=2' 4000)) {
    Fail 'drill did not re-enumerate the subfolder (rows=2 = back row + inner.txt)'
}
Start-Sleep -Milliseconds $SettleMs
Save-PanelShot 'panel_drilled.png' | Out-Null
Write-Host 'D  PASS  drill re-enumerated inside the panel (back row + 1 file)'
$report.Add('D  PASS  drill re-enumerated inside the panel (back row + 1 file)')

# ---- Part E: back ------------------------------------------------------------
Send-Cmd 'back'
# Root rows: [sub, empty, <long name>, alpha.txt] (folders first, then
# case-insensitive names; no back row at the root).
if (-not (Wait-LogLine 'reload\(back\) status=ok rows=4' 4000)) {
    Fail 'back did not return to the 4-row root listing'
}
Start-Sleep -Milliseconds $SettleMs
Save-PanelShot 'panel_back.png' | Out-Null
Write-Host 'E  PASS  back returned to the root level'
$report.Add('E  PASS  back returned to the root level')

# ---- Part F: ReadDirectoryChangesW refresh with 500ms debounce ---------------
$watchMarker = Join-Path $scratch ('watched-' + [Environment]::TickCount + '.txt')
$watchStart = [DateTime]::UtcNow
Set-Content -Path $watchMarker -Value 'x' -Encoding ASCII
if (-not (Wait-LogLine 'reload\(refresh\)' 4000)) {
    Fail 'watcher did not refresh the visible level'
}
$latency = (([DateTime]::UtcNow) - $watchStart).TotalMilliseconds
Write-Host ("F  PASS  ReadDirectoryChangesW refresh observed after {0:F0}ms (500ms debounce)" -f $latency)
$report.Add(("F  PASS  ReadDirectoryChangesW refresh observed after {0:F0}ms (500ms debounce)" -f $latency))

# ---- Part G: esc unwinds one level, then closes ------------------------------
# Part F's watcher refresh can land between the drill and the esc; the second
# drill targets the CURRENT folder, so sub is reached either way.
Start-Sleep -Milliseconds 600   # let any pending watcher refresh settle first
Send-Cmd 'drill sub'
Wait-LogLine 'reload\(drill\) status=ok' 4000 | Out-Null
Start-Sleep -Milliseconds 200
Send-Cmd 'esc'
if (-not (Wait-LogLine 'reload\(back\)' 4000)) {
    Fail 'esc at a sublevel did not unwind one level'
}
# The channel dedups an identical consecutive command, so a no-op goes between
# the two escs; 'noop' matches no handler and is only logged.
Start-Sleep -Milliseconds 300
Send-Cmd 'noop'
Start-Sleep -Milliseconds 300
Send-Cmd 'esc'
if (-not (Wait-LogLine 'panel closed' 4000)) {
    Fail 'esc at the root did not close the panel'
}
Write-Host 'G  PASS  esc unwinds one level first, closes at the root'
$report.Add('G  PASS  esc unwinds one level first, closes at the root')

# ---- Part H: REAL click outside closes the panel ----------------------------
# Reopen with a REAL dock click, then click outside IMMEDIATELY (~150ms): a
# panel opened from a dock click takes the foreground, but the system reclaims
# it for the previous app after ~400ms with no further input (measured: panel
# open at T, closed at T+399ms with no probe action in between). A real user's
# next click lands well inside that window.
Click-At ([int](($dk.Left + $dk.Right) / 2)) ([int]($dk.Bottom - 35))
Start-Sleep -Milliseconds 150
# A real click on open desktop wallpaper, far above the dock and the panel -
# the DoD's "click outside". Verified by t066_dock_click_diag: after a REAL
# dock click the panel holds the foreground and this desktop click takes it.
Click-At 100 200
Start-Sleep -Milliseconds 400
if ([T066Probe]::Find('DockForgeFolderPanel', $true) -ne [IntPtr]::Zero) {
    Start-Sleep -Milliseconds 600
    if ([T066Probe]::Find('DockForgeFolderPanel', $true) -ne [IntPtr]::Zero) {
        Fail 'panel still visible after a real click outside'
    }
}
Write-Host 'H  PASS  real click outside (SendInput) closed the panel'
$report.Add('H  PASS  real click outside (SendInput) closed the panel')

# ---- Part I: access-denied placeholder ---------------------------------------
Send-Cmd 'open C:\System Volume Information'
if (-not (Wait-LogLine 'panel open .*status=access-denied' 4000)) {
    Fail 'denied folder did not show the access-denied placeholder status'
}
Start-Sleep -Milliseconds $SettleMs
Save-PanelShot 'panel_denied.png' | Out-Null
Write-Host 'I  PASS  access-denied folder shows a placeholder, no crash'
$report.Add('I  PASS  access-denied folder shows a placeholder, no crash')

# ---- Part J: empty folder ----------------------------------------------------
Send-Cmd ('open ' + $empty)
if (-not (Wait-LogLine 'panel open .*status=ok rows=0' 4000)) {
    Fail 'empty folder did not render the empty body'
}
Start-Sleep -Milliseconds $SettleMs
Save-PanelShot 'panel_empty.png' | Out-Null
Write-Host 'J  PASS  empty folder shows the friendly empty body'
$report.Add('J  PASS  empty folder shows the friendly empty body')

# ---- Part K: drag-out (OLE IDataObject + CF_HDROP) ---------------------------
# A REAL SendInput drag from a panel row onto the DockForge dock window. The
# dock's production IDropTarget (T-014) accepts the payload only if the panel's
# OLE drag object actually carries a readable CF_HDROP; acceptance is decided
# by the log line the drop commit writes. No Explorer window is needed as the
# target: the dock IS a real OLE drop target.
function Count-OpenLines {
    $l = Latest-Log
    if (-not $l) { return 0 }
    return @(Select-String -Path $l.FullName -Pattern 'folder panel opened for tile' -ErrorAction SilentlyContinue).Count
}
Send-Cmd 'close'   # part J left the panel open on the empty folder
Start-Sleep -Milliseconds 300
$opensBefore = Count-OpenLines
$openedK = $false
foreach ($attempt in 1..3) {
    Click-At ([int](($dk.Left + $dk.Right) / 2)) ([int]($dk.Bottom - 35))
    $deadline = [DateTime]::UtcNow.AddMilliseconds(2500)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ((Count-OpenLines) -gt $opensBefore) { $openedK = $true; break }
        Start-Sleep -Milliseconds 100
    }
    if ($openedK) { break }
    Start-Sleep -Milliseconds 300
}
if (-not $openedK) { Fail 'dock tile click did not reopen the panel for the drag-out part' }
Start-Sleep -Milliseconds 150   # same ~400ms foreground-reclaim window as part H
$panel = [T066Probe]::Find('DockForgeFolderPanel', $true)
if ($panel -eq [IntPtr]::Zero) { Fail 'panel not visible for the drag-out part' }
$pr = New-Object T066Probe+RECT
[T066Probe]::GetWindowRect($panel, [ref]$pr) | Out-Null
Write-Host ("  drag: panel rect=({0},{1})-({2},{3})" -f $pr.Left, $pr.Top, $pr.Right, $pr.Bottom)
$dock = [T066Probe]::Find('DockForgeHostWindow', $true)
if ($dock -eq [IntPtr]::Zero) { Fail 'dock window not found for the drag-out target' }
$dr = New-Object T066Probe+RECT
[T066Probe]::GetWindowRect($dock, [ref]$dr) | Out-Null
# Row 0 of the root listing is the `sub` FOLDER (folders sort first): dragging a
# folder is accepted by the drop resolver (AppKind::Folder), unlike a .txt.
$scale = 2.0   # the panel was opened at the dock's DPI (192 on this run)
$srcX = ($pr.Left + $pr.Right) / 2
$srcY = $pr.Top + [int](34 * $scale) + [int](15 * $scale)
$dstX = ($dr.Left + $dr.Right) / 2
$dstY = ($dr.Top + $dr.Bottom) / 2
function Drag-Step([int]$px, [int]$py) {
    [T066Probe]::SetCursorPos($px, $py) | Out-Null
}
Drag-Step $srcX $srcY
Start-Sleep -Milliseconds 120
[T066Probe]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero) | Out-Null  # LEFTDOWN
Start-Sleep -Milliseconds 150
# Walk toward the dock in steps so the OLE loop and the dock's DragOver both
# see intermediate moves (a teleport can skip DragOver entirely).
foreach ($t in 1..8) {
    $mx = [int]($srcX + ($dstX - $srcX) * $t / 8.0)
    $my = [int]($srcY + ($dstY - $srcY) * $t / 8.0)
    Drag-Step $mx $my
    Start-Sleep -Milliseconds 80
}
Start-Sleep -Milliseconds 200
[T066Probe]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero) | Out-Null  # LEFTUP
Start-Sleep -Milliseconds 500
# Row 0 is whichever child folder sorts first (empty/sub). The decisive chain:
# DoDragDrop returned DRAGDROP_S_DROP, the dock's production drop target read
# the CF_HDROP (drop enter/commit), and the path was accepted as a tile.
$m = Wait-LogLine 'drag-out .*hr=0x00040100' 4000
$m2 = Wait-LogLine 'drop commit: 1 paths' 4000
$m3 = Wait-LogLine 'drop: added .*dockforge-t066-.+\\(sub|empty) at tile' 4000
if (-not $m -or -not $m2 -or -not $m3) {
    Fail ("drag-out did not land on the dock: " +
          ((Get-Content (Latest-Log).FullName | Select-String 'drop|drag-out') -join '; '))
}
Write-Host 'K  PASS  drag-out: panel folder dragged onto the dock, CF_HDROP accepted'
$report.Add('K  PASS  drag-out: panel folder dragged onto the dock, CF_HDROP accepted')

# ---- done --------------------------------------------------------------------
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
$report | Set-Content -Path (Join-Path $OutDir 'probe.txt') -Encoding UTF8
Write-Host '=== T-066 probe: ALL PASS ==='
exit 0
