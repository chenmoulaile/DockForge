# DockForge T-050 evidence probe: settings window open, edit -> debounce -> save ->
# publish chain, clamping echo, and light/dark screenshots.
#
# The run is config-isolated (DOCKFORGE_CONFIG_PATH under %TEMP%), so the values this
# probe writes never touch the real user config. `set` commands go through the same
# RecordEdit path the keyboard takes: debounce 300ms -> model apply+clamp -> SaveConfig
# -> ConfigChanged publish.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\settings_probe.ps1
param(
    [string]$OutDir = 'evidence\T-050',
    [string]$Exe = 'build\bin\DockForge.exe'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T050Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static IntPtr Find(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
[void][T050Probe]::SetProcessDpiAwarenessContext([IntPtr](-4))

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
    } finally { $fs.Close() }
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
        Start-Sleep -Milliseconds 50
    }
    return $null
}
function Save-WindowShot([string]$Path) {
    $hwnd = [T050Probe]::Find('DockForgeSettingsWindow')
    if ($hwnd -eq [IntPtr]::Zero) { Write-Output "no settings window for $Path"; return $false }
    $r = New-Object T050Probe+RECT
    [void][T050Probe]::GetWindowRect($hwnd, [ref]$r)
    $w = $r.Right - $r.Left
    $h = $r.Bottom - $r.Top
    # PrintWindow, not CopyFromScreen: the settings window can be occluded by
    # whatever the user has on screen (measured - a game captured instead of the
    # window), and PrintWindow pulls the window's own surface regardless of z-order.
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][T050Probe]::PrintWindow($hwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc)
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $true
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$report = New-Object System.Collections.Generic.List[string]
function Add-Report([string]$Line) {
    $script:report.Add($Line)
    Write-Output $Line
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$TempConfigDir = Join-Path $env:TEMP ('dockforge-t050-' + [Environment]::TickCount)
$TempConfigPath = Join-Path $TempConfigDir 'config.json'
$script:CmdFile = Join-Path $env:TEMP 'df_settings_probe_cmd.txt'
Set-Command ''
$runOffset = Get-LogLength

$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_SHOW = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $script:CmdFile
$env:DOCKFORGE_CONFIG_PATH = $TempConfigPath
$env:DOCKFORGE_LOG_LEVEL = 'debug'
Remove-Item Env:\DOCKFORGE_LAUNCHPAD_CMD_FILE -ErrorAction SilentlyContinue

function Start-App([string]$Theme) {
    $env:DOCKFORGE_SETTINGS_THEME = $Theme
    $script:proc = Start-Process -FilePath $Exe -PassThru
    $offset = Get-LogLength
    $ready = Wait-LogMatch $offset 'settings\] ready' 20000
    Start-Sleep -Milliseconds 1200
    return $offset
}

# --- screenshots: light + dark -------------------------------------------------
$null = Start-App 'light'
$shotLight = Save-WindowShot (Join-Path $OutDir 'settings_light.png')
Add-Report "shot_light=$shotLight"

$proc.Kill(); $proc.WaitForExit(); Start-Sleep -Milliseconds 400
$null = Start-App 'dark'
$shotDark = Save-WindowShot (Join-Path $OutDir 'settings_dark.png')
Add-Report "shot_dark=$shotDark"

# --- edit chain: set -> debounce -> save -> publish -----------------------------
$setOffset = Get-LogLength
Set-Command 'set dock.icon_size 64'
$flush = Wait-LogMatch $setOffset 'settings\] flush applied=\d+ changed=\d+ saved=1' 5000
$published = Wait-LogMatch $setOffset 'config published icon_size=64' 5000
Add-Report "set_64 flush=$([bool]$flush) published=$([bool]$published) line=$(if ($flush) { $flush.Trim() })"

Start-Sleep -Milliseconds 400
$savedValue = -1
if (Test-Path $TempConfigPath) {
    try {
        $cfg = Get-Content -Raw -Encoding UTF8 $TempConfigPath | ConvertFrom-Json
        $savedValue = [int]$cfg.dock.icon_size
    } catch { Add-Report "config_parse_error=$($_.Exception.Message)" }
}
Add-Report "config_icon_size=$savedValue (expected 64)"

# --- clamping: an over-range value lands on the range max and is echoed ---------
$clampOffset = Get-LogLength
Set-Command 'set dock.icon_size 9999'
$clampFlush = Wait-LogMatch $clampOffset 'settings\] flush applied=\d+ changed=\d+ saved=1' 5000
Start-Sleep -Milliseconds 400
$clampedValue = -1
if (Test-Path $TempConfigPath) {
    try {
        $cfg = Get-Content -Raw -Encoding UTF8 $TempConfigPath | ConvertFrom-Json
        $clampedValue = [int]$cfg.dock.icon_size
    } catch { }
}
Add-Report "clamp_9999 flush=$([bool]$clampFlush) config_icon_size=$clampedValue (expected 96)"

$proc.Kill(); $proc.WaitForExit()
try { Remove-Item -Recurse -Force $TempConfigDir } catch { }

# --- verdict --------------------------------------------------------------------
$fail = @()
if (-not $shotLight) { $fail += 'light screenshot missing (window not found)' }
if (-not $shotDark) { $fail += 'dark screenshot missing (window not found)' }
if (-not $flush) { $fail += 'set 64 never reached the flush log line' }
if (-not $published) { $fail += 'ConfigChanged was never published (C-20)' }
if ($savedValue -ne 64) { $fail += "config holds icon_size=$savedValue, expected 64" }
if (-not $clampFlush) { $fail += 'the over-range edit never flushed' }
if ($clampedValue -ne 96) { $fail += "clamp wrote $clampedValue, expected the range max 96" }

if ($fail.Count -eq 0) {
    Add-Report 'verdict=PASS'
} else {
    Add-Report "verdict=FAIL reasons=$($fail -join '; ')"
}
$report | Set-Content -Path (Join-Path $OutDir 'settings_probe.txt') -Encoding UTF8
if ($fail.Count -gt 0) { exit 1 }
