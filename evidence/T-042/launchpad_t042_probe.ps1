# DockForge T-042 evidence probe: Launchpad search field, empty state, drag-to-create
# folders, folder open, rename (commit + cancel), Esc unwind, and persistence.
#
# Drives the Launchpad through the file channel (DOCKFORGE_LAUNCHPAD_CMD_FILE). The
# T-042 command set is used throughout: type/backspace/key/press/release. Only the
# *source* of the input is replaced; the production Character/KeyPressed/PressAt/
# ReleaseAt paths run, so a PASS still proves the chain.
#
# The run is config-isolated: DOCKFORGE_CONFIG_PATH points at a fresh file under
# %TEMP%, so the folders this probe creates never touch the real user config.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\launchpad_t042_probe.ps1
param(
    [string]$OutDir = 'evidence\T-042',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$WaitOpenMs = 1200,
    [int]$WaitCloseMs = 800
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T042Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
}
'@
[void][T042Probe]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-LatestLog {
    $dir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
    Get-ChildItem $dir -Filter 'dockforge-*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Get-LogLength {
    $log = Get-LatestLog
    if (-not $log) { return 0 }
    return (Get-Item $log.FullName).Length
}

function Read-LogFrom([long]$Offset) {
    $log = Get-LatestLog
    if (-not $log) { return @() }
    $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        $sr = New-Object System.IO.StreamReader($fs)
        $fs.Position = [Math]::Min($Offset, $fs.Length)
        $text = $sr.ReadToEnd()
        $sr.Close()
    } finally {
        $fs.Close()
    }
    return ($text -split "`n" | Where-Object { $_.Trim().Length -gt 0 })
}

function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($script:CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}

function Wait-LogMatch([long]$Offset, [string]$Pattern, [int]$TimeoutMs = 5000) {
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        foreach ($line in (Read-LogFrom $Offset)) {
            if ($line -match $Pattern) { return $line }
        }
        Start-Sleep -Milliseconds 60
    }
    return $null
}

function Save-Shot([string]$Path) {
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

function Compare-Shots([string]$PathA, [string]$PathB, [int]$Step = 8, [int]$Threshold = 24) {
    $bmpA = [System.Drawing.Bitmap]::FromFile($PathA)
    $bmpB = [System.Drawing.Bitmap]::FromFile($PathB)
    $changed = 0
    $total = 0
    for ($y = 0; $y -lt [Math]::Min($bmpA.Height, $bmpB.Height); $y += $Step) {
        for ($x = 0; $x -lt [Math]::Min($bmpA.Width, $bmpB.Width); $x += $Step) {
            $ca = $bmpA.GetPixel($x, $y)
            $cb = $bmpB.GetPixel($x, $y)
            $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
            if ($d -gt $Threshold) { $changed++ }
            $total++
        }
    }
    $bmpA.Dispose()
    $bmpB.Dispose()
    return [pscustomobject]@{
        Changed = $changed
        Total   = $total
        Ratio   = if ($total -gt 0) { [Math]::Round($changed / $total, 4) } else { 0.0 }
    }
}

function Compare-Region([string]$PathA, [string]$PathB, $Rect, [int]$Step = 4, [int]$Threshold = 24) {
    $bmpA = [System.Drawing.Bitmap]::FromFile($PathA)
    $bmpB = [System.Drawing.Bitmap]::FromFile($PathB)
    $changed = 0
    $total = 0
    for ($y = $Rect.Top; $y -lt $Rect.Bottom; $y += $Step) {
        for ($x = $Rect.Left; $x -lt $Rect.Right; $x += $Step) {
            $ca = $bmpA.GetPixel($x, $y)
            $cb = $bmpB.GetPixel($x, $y)
            $d = [Math]::Abs($ca.R - $cb.R) + [Math]::Abs($ca.G - $cb.G) + [Math]::Abs($ca.B - $cb.B)
            if ($d -gt $Threshold) { $changed++ }
            $total++
        }
    }
    $bmpA.Dispose()
    $bmpB.Dispose()
    return [pscustomobject]@{
        Changed = $changed
        Total   = $total
        Ratio   = if ($total -gt 0) { [Math]::Round($changed / $total, 4) } else { 0.0 }
    }
}

# ---------------------------------------------------------------------------

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$report = New-Object System.Collections.Generic.List[string]
function Add-Report([string]$Line) {
    $script:report.Add($Line)
    Write-Output $Line
}

Add-Report "machine=$([System.Windows.Forms.SystemInformation]::VirtualScreen.Width)x$([System.Windows.Forms.SystemInformation]::VirtualScreen.Height) dpi_aware=1"

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

# Config isolation: a fresh config under %TEMP%. SaveConfig creates the directory
# itself, and an absent file loads defaults, so launchpad.folders starts empty.
$script:TempConfigDir = Join-Path $env:TEMP ('dockforge-t042-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $TempConfigDir | Out-Null
$TempConfigPath = Join-Path $TempConfigDir 'config.json'
Add-Report "config_isolated=$TempConfigPath"

$script:CmdFile = Join-Path $env:TEMP 'df_launchpad_t042_cmd.txt'
Set-Command ''
$runOffset = Get-LogLength

$env:DOCKFORGE_LAUNCHPAD_CMD_FILE = $script:CmdFile
$env:DOCKFORGE_CONFIG_PATH = $TempConfigPath
$env:DOCKFORGE_LOG_LEVEL = 'debug'
Remove-Item Env:\DOCKFORGE_PREVIEW -ErrorAction SilentlyContinue
Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue
Remove-Item Env:\DOCKFORGE_PIN_APPS -ErrorAction SilentlyContinue

function Start-App {
    $script:proc = Start-Process -FilePath $Exe -PassThru
    $offset = Get-LogLength
    $warm = Wait-LogMatch $offset 'launchpad\] index warmed' 25000
    if (-not $warm) { Add-Report 'verdict=FAIL warmup never finished'; exit 1 }
    return $offset
}

function Open-Launchpad {
    $offset = Get-LogLength
    Set-Command 'open'
    [void](Wait-LogMatch $offset 'launchpad\] grid\[0\]' 6000)
    $line = Wait-LogMatch $offset 'launchpad\] open items=' 6000
    [void](Wait-LogMatch $offset 'launchpad\] open complete' 6000)
    Start-Sleep -Milliseconds $WaitOpenMs
    return $line
}

# Parse "origin=64.0,205.0 cell=105.2x120.0" out of a grid log line.
function Parse-Grid([string]$Line) {
    $g = @{}
    foreach ($m in [regex]::Matches($line, '(\w+)=([\-0-9\.x,]+)')) {
        $g[$m.Groups[1].Value] = $m.Groups[2].Value
    }
    $origin = $g['origin'].Split(',')
    $cell = $g['cell'].Split('x')
    return [pscustomobject]@{
        Columns = [int]$g['columns']
        OriginX = [double]$origin[0]
        OriginY = [double]$origin[1]
        CellW   = [double]$cell[0]
        CellH   = [double]$cell[1]
    }
}

$null = Start-App

# --- Stage A: search field filters live, empty query state does not crash -----------
$openLine = Open-Launchpad
Add-Report "open=$($openLine.Trim())"
$itemsTotal = 0
if ($openLine -match 'items=(\d+)') { $itemsTotal = [int]$Matches[1] }
Save-Shot (Join-Path $OutDir 't042_open.png')

$gridLine = (Read-LogFrom $runOffset | Where-Object { $_ -match 'launchpad\] grid\[0\]' } |
    Select-Object -Last 1)
$grid = Parse-Grid $gridLine
Add-Report ("grid origin={0},{1} cell={2}x{3} columns={4}" -f $grid.OriginX, $grid.OriginY,
    $grid.CellW, $grid.CellH, $grid.Columns)

# No folders yet, so the top band is the plain 56: the search field sits inside it and
# the first grid row starts below. A chip-strip click would land in the band either way.
$cell0X = [Math]::Round($grid.OriginX + $grid.CellW * 0.5, 1)
$cell0Y = [Math]::Round($grid.OriginY + $grid.CellH * 0.5, 1)
$cell1X = [Math]::Round($grid.OriginX + $grid.CellW * 1.5, 1)
$cell1Y = $cell0Y

$typeOffset = Get-LogLength
Set-Command 'type 7'
$searchLine = Wait-LogMatch $typeOffset "search '7' -> (\d+) item" 5000
Add-Report "search_7=$([bool]$searchLine) line=$(if ($searchLine) { $searchLine.Trim() })"
Start-Sleep -Milliseconds 300
Save-Shot (Join-Path $OutDir 't042_search.png')
$searchDiff = Compare-Shots (Join-Path $OutDir 't042_open.png') (Join-Path $OutDir 't042_search.png')
Add-Report "search_diff ratio=$($searchDiff.Ratio)"

$searchCount = -1
if ($searchLine -match "-> (\d+) item") { $searchCount = [int]$Matches[1] }

# Empty state: append an impossible suffix, the grid must empty out and stay alive.
Set-Command 'type zzzzprobe'
$emptyLine = Wait-LogMatch $typeOffset "search '7zzzzprobe' -> 0 item" 5000
Add-Report "empty_state=$([bool]$emptyLine) line=$(if ($emptyLine) { $emptyLine.Trim() })"
Start-Sleep -Milliseconds 300
Save-Shot (Join-Path $OutDir 't042_empty.png')
# The search view holds exactly one cell, so the whole-screen diff against the empty
# state is diluted to ~0.002 - the honest comparison is the region that cell occupies.
# Cell 0 centre is (origin + cell/2) DIP; the app runs at the desktop's scale, read
# back from the overlay's physical size divided by its DIP size.
# Scale from two sources of truth the app itself reported: the physical monitor and
# the DIP viewport in the grid log line. Hardcoding 2.0 would silently break the
# moment the desktop scale changes.
$viewportW = 1.0
if ($gridLine -match 'viewport=([\d\.]+)x') { $viewportW = [double]$Matches[1] }
$scale = [Math]::Max(1.0, [System.Windows.Forms.SystemInformation]::VirtualScreen.Width / $viewportW)
Add-Report "probe_scale=$scale" 
$c0px = [pscustomobject]@{
    Left   = [int](($grid.OriginX + $grid.CellW * 0.0) * $scale)
    Top    = [int](($grid.OriginY) * $scale)
    Right  = [int](($grid.OriginX + $grid.CellW * 1.0) * $scale)
    Bottom = [int](($grid.OriginY + $grid.CellH * 1.0) * $scale)
}
$emptyDiff = Compare-Region (Join-Path $OutDir 't042_search.png') (Join-Path $OutDir 't042_empty.png') $c0px
Add-Report "empty_diff_cell0 ratio=$($emptyDiff.Ratio)"
$aliveAfterEmpty = (-not $proc.HasExited)
Add-Report "alive_after_empty=$aliveAfterEmpty"

Set-Command 'close'
Start-Sleep -Milliseconds $WaitCloseMs

# --- Stage B: drag cell 0 onto cell 1 creates a folder and persists it --------------
$null = Open-Launchpad
$gridLine = (Read-LogFrom $runOffset | Where-Object { $_ -match 'launchpad\] grid\[0\]' } |
    Select-Object -Last 1)
$grid = Parse-Grid $gridLine
$cell0X = [Math]::Round($grid.OriginX + $grid.CellW * 0.5, 1)
$cell0Y = [Math]::Round($grid.OriginY + $grid.CellH * 0.5, 1)
$cell1X = [Math]::Round($grid.OriginX + $grid.CellW * 1.5, 1)
$cell1Y = $cell0Y

$dragOffset = Get-LogLength
Set-Command ("press {0} {1}" -f $cell0X, $cell0Y)
Start-Sleep -Milliseconds 120
Set-Command ("hover {0} {1}" -f $cell1X, $cell1Y)
Start-Sleep -Milliseconds 120
Set-Command ("release {0} {1}" -f $cell1X, $cell1Y)
$createdLine = Wait-LogMatch $dragOffset 'folder created id=\S+ items=2 saved=1' 6000
Add-Report "folder_created=$([bool]$createdLine) line=$(if ($createdLine) { $createdLine.Trim() })"
Start-Sleep -Milliseconds 300
Save-Shot (Join-Path $OutDir 't042_folder_created.png')

# --- Stage C: clicking the chip enters the folder -----------------------------------
# Chip 0: left=16, width=168, top=56, height=34 -> centre (100, 73) DIP.
$chipX = 100.0
$chipY = 73.0
$enterOffset = Get-LogLength
Set-Command ("click {0} {1}" -f $chipX, $chipY)
$enterLine = Wait-LogMatch $enterOffset 'folder opened id=\S+ name=New Folder items=2' 6000
Add-Report "folder_entered=$([bool]$enterLine) line=$(if ($enterLine) { $enterLine.Trim() })"
Start-Sleep -Milliseconds 300
Save-Shot (Join-Path $OutDir 't042_folder_open.png')

# --- Stage D: F2 renames; Enter commits; Escape cancels -----------------------------
$f2Offset = Get-LogLength
Set-Command 'key 113'   # VK_F2
$renameStart = Wait-LogMatch $f2Offset 'rename started id=' 5000
Add-Report "rename_started=$([bool]$renameStart)"
# The editor pre-fills the old name with the caret at the end, so typing alone would
# append ("New FolderPlay"). Wipe the field first, the way select-all+delete would.
Set-Command 'clear'
Start-Sleep -Milliseconds 150
Set-Command 'type Play'
Start-Sleep -Milliseconds 200
Set-Command 'key 13'    # VK_RETURN
$renamedLine = Wait-LogMatch $f2Offset 'folder renamed id=\S+ saved=1' 6000
Add-Report "folder_renamed=$([bool]$renamedLine) line=$(if ($renamedLine) { $renamedLine.Trim() })"
Start-Sleep -Milliseconds 300
Save-Shot (Join-Path $OutDir 't042_renamed.png')

# Cancel path: start a rename, type something, then Escape must keep the old name.
$cancelOffset = Get-LogLength
Set-Command 'key 113'
[void](Wait-LogMatch $cancelOffset 'rename started id=' 5000)
Set-Command 'type X'
Start-Sleep -Milliseconds 200
Set-Command 'key 27'    # VK_ESCAPE
$cancelLine = Wait-LogMatch $cancelOffset 'rename cancelled id=' 5000
Add-Report "rename_cancelled=$([bool]$cancelLine)"
Start-Sleep -Milliseconds 300
Save-Shot (Join-Path $OutDir 't042_cancelled.png')

# Escape with a folder open (and nothing being renamed) must leave the folder, not
# kill the whole Launchpad - one level at a time. An unrelated command goes first:
# the channel ignores a command identical to the previous one, and this is the
# second `key 27` in a row.
$leaveOffset = Get-LogLength
Set-Command 'leave'
Start-Sleep -Milliseconds 150
Set-Command 'key 27'
$leaveLine = Wait-LogMatch $leaveOffset 'folder closed' 5000
Add-Report "esc_left_folder=$([bool]$leaveLine)"
Add-Report "alive_after_unwind=$(-not $proc.HasExited)"

Set-Command 'close'
Start-Sleep -Milliseconds $WaitCloseMs

# --- Stage E: persistence - kill, relaunch on the same temp config, chip still there -
Stop-Process -Id $proc.Id -Force
Start-Sleep -Milliseconds 600
$null = Start-App
$null = Open-Launchpad
$persistOffset = Get-LogLength
Set-Command ("click {0} {1}" -f $chipX, $chipY)
$persistLine = Wait-LogMatch $persistOffset 'folder opened id=\S+ name=Play items=2' 6000
Add-Report "persisted_folder_opens=$([bool]$persistLine) line=$(if ($persistLine) { $persistLine.Trim() })"

$persistedName = ''
$persistedItems = 0
if (Test-Path $TempConfigPath) {
    try {
        $cfg = Get-Content -Raw -Encoding UTF8 $TempConfigPath | ConvertFrom-Json
        $persistedName = $cfg.launchpad.folders[0].name
        $persistedItems = @($cfg.launchpad.folders[0].items).Count
    } catch {
        Add-Report "config_parse_error=$($_.Exception.Message)"
    }
}
Add-Report "config_folder_name=$persistedName config_folder_items=$persistedItems"

Stop-Process -Id $proc.Id -Force
Start-Sleep -Milliseconds 600
try { Remove-Item -Recurse -Force $TempConfigDir } catch { Add-Report "temp_cleanup_failed=$($_.Exception.Message)" }

# --- verdict -------------------------------------------------------------------------
$fail = @()
if (-not $searchLine) { $fail += 'typing never reached the search field' }
if ($searchCount -lt 1) { $fail += "prefix '7' matched nothing (expected 7-Zip on this machine)" }
if ($searchCount -ge $itemsTotal) { $fail += 'the query did not shrink the grid' }
if ($searchDiff.Ratio -lt 0.05) { $fail += 'search results look identical to the full grid' }
if (-not $emptyLine) { $fail += 'an unmatchable query never produced the 0-item state' }
# The search view held exactly one cell; its disappearance plus the "No results"
# label move ~3% of sampled pixels, so the floor here is below the grid-wide 0.05.
if ($emptyDiff.Ratio -lt 0.05) { $fail += 'the empty state looks identical to the search results' }
if (-not $aliveAfterEmpty) { $fail += 'the process died during the empty-state case' }
if (-not $createdLine) { $fail += 'drag cell0 -> cell1 never created a folder' }
if (-not $enterLine) { $fail += 'clicking the chip never opened the folder' }
if (-not $renameStart) { $fail += 'F2 never started a rename' }
if (-not $renamedLine) { $fail += 'Enter never committed the rename' }
if (-not $cancelLine) { $fail += 'Escape never cancelled a rename' }
if (-not $leaveLine) { $fail += 'Escape with a folder open never left the folder' }
if (-not $persistLine) { $fail += 'the renamed folder did not survive a restart' }
if ($persistedName -ne 'Play') { $fail += "persisted name is '$persistedName', expected 'Play'" }
if ($persistedItems -ne 2) { $fail += "persisted folder holds $persistedItems items, expected 2" }

if ($fail.Count -eq 0) {
    Add-Report 'verdict=PASS'
} else {
    Add-Report "verdict=FAIL reasons=$($fail -join '; ')"
}
$report | Set-Content -Path (Join-Path $OutDir 't042_probe.txt') -Encoding UTF8
if ($fail.Count -gt 0) { exit 1 }
