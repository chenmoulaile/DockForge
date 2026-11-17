# T-307 evidence capture: run the built app with a scratch config that has
# clock + weather_brief in the Finder Bar right region, open the calendar panel
# through the probe channel, and screenshot the desktop.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools/t307_evidence.ps1 -Theme dark|light
param(
    [Parameter(Mandatory=$true)][ValidateSet("dark","light")][string]$Theme,
    [string]$OutDir = "evidence/T-307",
    # fail: point DOCKFORGE_WEATHER_API_HOST at a dead name (the T-075 documented
    # failure-path probe) so the entry shows the "--°" placeholder.
    # popup: also open the 3-day forecast popup and screenshot it.
    [ValidateSet("ok","fail")][string]$WeatherMode = "ok",
    [switch]$Popup,
    [switch]$EscCheck
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $repo "build\bin\DockForge.exe"
if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

$scratch = Join-Path $repo "evidence\T-307\scratch"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $repo $OutDir) | Out-Null

# Scratch config: clock at 0 (rightmost), weather_brief at 1 (left of the clock).
# weather.enabled + city so the shared T-075 host actually runs; if the network is
# unreachable the entry shows the "--°" placeholder, which is itself documented DoD
# behaviour ("no data shows a placeholder").
$configPath  = Join-Path $scratch "config_$Theme.json"
$cmdFile     = Join-Path $scratch "cmd_$Theme.txt"
$stateFile   = Join-Path $scratch "state_$Theme.txt"
@'
{
  "weather": { "enabled": true, "provider": "open_meteo", "city": "Beijing", "refresh_minutes": 30 },
  "finderbar": {
    "enabled": true,
    "height": 28,
    "right": [
      { "type": "clock", "order": 0 },
      { "type": "weather_brief", "order": 1 }
    ]
  }
}
'@ | Set-Content -Path $configPath -Encoding UTF8

Remove-Item -Force -ErrorAction SilentlyContinue $cmdFile, $stateFile
Set-Content -Path $cmdFile -Value "" -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH      = $configPath
$env:DOCKFORGE_FINDERBAR        = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE   = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL   = "solid"
if ($WeatherMode -eq "fail") {
    $env:DOCKFORGE_WEATHER_API_HOST = "weather.invalid-dockforge.test"
} else {
    Remove-Item Env:DOCKFORGE_WEATHER_API_HOST -ErrorAction SilentlyContinue
}

$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 8   # first fetch + first layout + clock text

# The state file is written on every non-empty probe command, so ask for "state"
# first (a no-op command that just publishes the snapshot JSON).
Set-Content -Path $cmdFile -Value "state" -Encoding ASCII
Start-Sleep -Seconds 2
if (-not (Test-Path $stateFile)) { throw "state file was not written: $stateFile" }
$suffix = if ($WeatherMode -eq "fail") { "_weatherfail" } else { "" }
Copy-Item $stateFile (Join-Path $repo "$OutDir\state_$Theme$suffix.txt") -Force
$stateRaw = Get-Content $stateFile -Raw
$stateDpi = if ($stateRaw -match '"dpi":([0-9]+)') { [int]$Matches[1] } else { 96 }
$stateBarHeight = if ($stateRaw -match '"height_dip":([0-9.]+)') { [double]$Matches[1] } else { 28.0 }

# The MouseClick helper needs to exist before EscCheck uses it.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class MouseClick {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint data, UIntPtr extra);
    public static void LeftClick(int x, int y) {
        SetProcessDPIAware();  // physical-pixel coordinates on this 192 DPI host
        SetCursorPos(x, y);
        System.Threading.Thread.Sleep(120);
        mouse_event(0x0002, 0, 0, 0, UIntPtr.Zero);
        mouse_event(0x0004, 0, 0, 0, UIntPtr.Zero);
    }
}
"@

# Open the calendar panel through the real click path's code (probe command).
Set-Content -Path $cmdFile -Value "calendar" -Encoding ASCII
Start-Sleep -Seconds 2

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
# This host runs at 192 DPI (scale 2). Without a DPI-aware declaration PowerShell
# gets virtualized bounds and CopyFromScreen captures half the desktop.
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class DpiAware {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[DpiAware]::SetProcessDPIAware() | Out-Null
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
Write-Host "screen bounds: $($bounds.Width)x$($bounds.Height)"
$bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$png = Join-Path $repo "$OutDir\bar_calendar_$Theme$suffix.png"
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $png ($($bounds.Width)x$($bounds.Height))"

# Also capture the bar's right corner strip for a close-up (top 56 px, right 720 px).
$cropW = 720; $cropH = 56
if ($bounds.Width -lt $cropW) { $cropW = $bounds.Width }
$corner = New-Object System.Drawing.Bitmap $cropW, $cropH
$gc = [System.Drawing.Graphics]::FromImage($corner)
$gc.CopyFromScreen($bounds.Width - $cropW, 0, 0, 0, (New-Object System.Drawing.Size $cropW, $cropH))
$cornerPng = Join-Path $repo "$OutDir\bar_corner_$Theme$suffix.png"
$corner.Save($cornerPng, [System.Drawing.Imaging.ImageFormat]::Png)
$gc.Dispose(); $corner.Dispose()
Write-Host "saved $cornerPng"

if ($Popup) {
    # Close the calendar first (a second probe "calendar" toggles it off), then open
    # the forecast popup. The probe gate ignores a content-identical file, so an
    # intermediate "state" command makes the toggle "calendar" a new content.
    Set-Content -Path $cmdFile -Value "state" -Encoding ASCII
    Start-Sleep -Milliseconds 900
    Set-Content -Path $cmdFile -Value "calendar" -Encoding ASCII
    Start-Sleep -Milliseconds 900
    Set-Content -Path $cmdFile -Value "weather" -Encoding ASCII
    Start-Sleep -Seconds 2
    $bmp2 = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g2 = [System.Drawing.Graphics]::FromImage($bmp2)
    $g2.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $png2 = Join-Path $repo "$OutDir\weather_popup_$Theme$suffix.png"
    $bmp2.Save($png2, [System.Drawing.Imaging.ImageFormat]::Png)
    $g2.Dispose(); $bmp2.Dispose()
    Write-Host "saved $png2"
}

if ($EscCheck) {
    # Click-outside dismissal through the REAL user path (no probe command): a real
    # mouse click on the clock box makes the bar window foreground, so the panel's
    # SetForegroundWindow succeeds (a process that owns the foreground may move it to
    # its own window); the panel then holds activation, and a second real click on
    # empty desktop fires WM_ACTIVATE(WA_INACTIVE) and closes it. A probe command
    # cannot prove this chain: with no window of this process in the foreground the
    # SetForegroundWindow call is refused and the never-activated panel must not
    # self-close on the first stray deactivate (hardware-panel rule).
    $snap = Get-Content $stateFile -Raw
    # Clock box centre, from the probe state (DIP -> physical px at scale=dpi/96).
    # nlohmann::json serializes object keys alphabetically, so the clock box looks
    # like {"icon":false,"left":...,"right":...,"text":"...","type":"clock"}.
    if ($snap -match '"left":([0-9.]+),"right":([0-9.]+),"text":"[^"]*","type":"clock"') {
        $clockLeftDip = [double]$Matches[1]; $clockRightDip = [double]$Matches[2]
        $dpiScale = $stateDpi / 96.0
        $clockX = [int]((($clockLeftDip + $clockRightDip) / 2.0) * $dpiScale)
        $clockY = [int](($stateBarHeight / 2.0) * $dpiScale)
        Write-Host "clicking clock box at $clockX,$clockY (toggle: panel is already open)"
        [MouseClick]::LeftClick($clockX, $clockY)   # close the open panel
        Start-Sleep -Seconds 1
        [MouseClick]::LeftClick($clockX, $clockY)   # open it again through the real path
        Start-Sleep -Seconds 2
        # Now a real click on empty desktop (physical px, mid-wallpaper).
        [MouseClick]::LeftClick(1400, 1100)
        Start-Sleep -Milliseconds 900
        Set-Content -Path $cmdFile -Value "state" -Encoding ASCII
        Start-Sleep -Seconds 1
        Copy-Item $stateFile (Join-Path $repo "$OutDir\state_$Theme$suffix.txt") -Force
        $snap2 = Get-Content $stateFile -Raw
        $vis = if ($snap2 -match '"calendar_visible":(true|false)') { $Matches[1] } else { "unknown" }
        Write-Host "calendar_visible after outside click: $vis"
    } else {
        Write-Host "clock box not found in state; EscCheck skipped"
    }
}

Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Write-Host "T-307 evidence ($Theme) done"
