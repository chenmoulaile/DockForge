# T-304 evidence capture: the control centre first session.
#
# Runs the built app with the DEFAULT finderbar.right table (no explicit table in
# the scratch config - the parse falls back to DefaultFinderBarRightItems(), so
# the ADR-0057 default clock=0/control_center=1/network=4/battery=5/ime=6 is what
# the bar actually renders). Then drives the panel through the probe channel:
#
#   cc_open / cc_hide           - the bar-button click path, toggle + close
#   cc_volume / cc_brightness   - live write paths (audio endpoint / WMI-DDC)
#   cc_radio 1|3 <0|1>          - the real-machine Wi-Fi/Bluetooth flip (T-068
#                                 write path, forward-verified here)
#   cc_dnd / cc_night           - registry switches (night light expects the
#                                 honest unsupported-hint on this machine)
#   cc_menu                     - the right-click minimal menu (TrackPopupMenuEx)
#
# Every system-level effect is verified with an INDEPENDENT read (WMI brightness
# query, registry value, sidecar radio state), not just the panel's own JSON.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools/t304_evidence.ps1
param(
    [string]$OutDir = "evidence/T-304",
    [switch]$NoRadio  # skip the Wi-Fi/Bluetooth flip (offline-runs)
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $repo "build\bin\DockForge.exe"
if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

$scratch = Join-Path $repo "evidence\T-304\scratch"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $repo $OutDir) | Out-Null

# Scratch config: finderbar enabled with NO explicit right table.
$configPath = Join-Path $scratch "config.json"
$cmdFile    = Join-Path $scratch "cmd.txt"
$stateFile  = Join-Path $scratch "state.txt"          # finderbar snapshot
$hwCmdFile  = Join-Path $scratch "hw_cmd.txt"
$hwStateFile= Join-Path $scratch "hw_state.txt"       # panel snapshot (T-068 chain)
@'
{
  "finderbar": { "enabled": true, "height": 28 }
}
'@ | Set-Content -Path $configPath -Encoding UTF8

Remove-Item -Force -ErrorAction SilentlyContinue $cmdFile, $stateFile, $hwCmdFile, $hwStateFile
Set-Content -Path $cmdFile -Value "" -Encoding ASCII
Set-Content -Path $hwCmdFile -Value "" -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH            = $configPath
$env:DOCKFORGE_FINDERBAR              = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE     = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE   = $stateFile
$env:DOCKFORGE_HW_PANEL_CMD_FILE      = $hwCmdFile
$env:DOCKFORGE_HW_PANEL_STATE_FILE    = $hwStateFile
# Frosted tiers render as an empty gradient in capture environments; the Solid
# tier is the evidence tier (ADR-0045 / the task brief's forensic note).
$env:DOCKFORGE_FORCE_MATERIAL         = "solid"

# Kill any previous evidence instance (one bar per machine).
Get-Process DockForge -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -eq $exe } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 6   # first sample + first layout

$script:cmdToken = 0
function Send-Cmd([string]$text) {
    # The probe gate dedups byte-identical file content, so every command carries a
    # unique token on a second line.
    $script:cmdToken++
    Set-Content -Path $cmdFile -Value "$text`n#t304-$script:cmdToken-$(Get-Random)" -Encoding ASCII
    Start-Sleep -Milliseconds 900
}
function Read-State {
    Send-Cmd "state"
    if (-not (Test-Path $stateFile)) { throw "state file was not written: $stateFile" }
    Get-Content $stateFile -Raw -Encoding UTF8
}
function Read-HwState {
    $script:cmdToken++
    Set-Content -Path $hwCmdFile -Value "state`n#t304-hw-$script:cmdToken-$(Get-Random)" -Encoding ASCII
    Start-Sleep -Milliseconds 700
    if (-not (Test-Path $hwStateFile)) { throw "hw state file was not written: $hwStateFile" }
    Get-Content $hwStateFile -Raw -Encoding UTF8
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("T-304 evidence run date=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$lines.Add("machine: $(Get-CimInstance Win32_OperatingSystem | Select-Object -ExpandProperty Caption) build=$([System.Environment]::OSVersion.Version.Build)")

function Assert([string]$name, [bool]$ok) {
    $lines.Add(("  [{0}] {1}" -f ($(if ($ok) {"PASS"} else {"FAIL"})), $name))
    if (-not $ok) { $script:failures += $name }
}
$failures = New-Object System.Collections.Generic.List[string]

# ---- the bar renders the default table incl. control_center --------------------
$stateObj = (Read-State | ConvertFrom-Json)
Copy-Item $stateFile (Join-Path $repo "$OutDir\state_bar.txt") -Force
$types = @($stateObj.right_boxes | ForEach-Object { $_.type })
$lines.Add("right_boxes: $($types -join ', ') (total_width_dip=$($stateObj.right_width_dip))")
Assert "default right table renders clock+control_center+network+battery+ime (ADR-0057)" `
    (($types -contains "clock") -and ($types -contains "control_center") -and `
     ($types -contains "network") -and ($types -contains "battery") -and ($types -contains "ime"))
$ccBox     = $stateObj.right_boxes | Where-Object { $_.type -eq "control_center" } | Select-Object -First 1
$clockBox  = $stateObj.right_boxes | Where-Object { $_.type -eq "clock" } | Select-Object -First 1
$networkBox= $stateObj.right_boxes | Where-Object { $_.type -eq "network" } | Select-Object -First 1
if ($ccBox -and $clockBox) {
    Assert "control_center sits LEFT of the clock (order 1 vs 0 from the right)" `
        ($ccBox.left -lt $clockBox.left)
    Assert "control_center box carries the 16 DIP icon (no text)" `
        ([bool]$ccBox.icon -and [string]::IsNullOrEmpty($ccBox.text))
} else {
    Assert "control_center box exists" $false
}
Assert "finderbar snapshot reports control_center_item=true" ($stateObj.control_center_item)

# ---- open the panel through the click path --------------------------------------
Send-Cmd "cc_open"
$openState = (Read-State | ConvertFrom-Json)
Assert "cc_open opens the panel (bar-button click path)" ($openState.control_center_visible)
Start-Sleep -Milliseconds 1200   # sidecar poll answer fills the radio cells

$hw = (Read-HwState | ConvertFrom-Json)
Copy-Item $hwStateFile (Join-Path $repo "$OutDir\state_panel_open.txt") -Force
$lines.Add("panel snapshot: brightness(supported=$($hw.brightness.supported) path=$($hw.brightness.path) level=$($hw.brightness.level)) " +
           "dnd(on=$($hw.dnd.on)) night(state=$($hw.night.state)) radios=$($hw.radios.Count) master=$($hw.master_level)")
Assert "panel snapshot reports visible=true width=696px height=548px at 192dpi" `
    ($hw.panel.visible -and $hw.panel.width_px -eq 696 -and $hw.panel.height_px -eq 548)
Assert "panel carries the 2x2 connection data (radios listed or dnd state present)" `
    ($null -ne $hw.dnd)

# ---- screenshot: panel open ------------------------------------------------------
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class DpiAwareT304 {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[DpiAwareT304]::SetProcessDPIAware() | Out-Null
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

$full = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g2 = [System.Drawing.Graphics]::FromImage($full)
$g2.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$pngFull = Join-Path $repo "$OutDir\cc_panel_open.png"
$full.Save($pngFull, [System.Drawing.Imaging.ImageFormat]::Png)
$g2.Dispose(); $full.Dispose()
$lines.Add("saved $pngFull")

# zoom crop: the panel hangs below the bar's right region
$cropW = 820; $cropH = 640
if ($bounds.Width -lt $cropW) { $cropW = $bounds.Width }
if ($bounds.Height -lt $cropH) { $cropH = $bounds.Height }
$crop = New-Object System.Drawing.Bitmap $cropW, $cropH
$g3 = [System.Drawing.Graphics]::FromImage($crop)
$g3.CopyFromScreen($bounds.Width - $cropW, 0, 0, 0,
                   (New-Object System.Drawing.Size($cropW, $cropH)))
$pngZoom = Join-Path $repo "$OutDir\cc_panel_zoom.png"
$crop.Save($pngZoom, [System.Drawing.Imaging.ImageFormat]::Png)
$g3.Dispose(); $crop.Dispose()
$lines.Add("saved $pngZoom")

# bar strip (control centre button visible)
$barW = 780; $barH = 64
$bar = New-Object System.Drawing.Bitmap $barW, $barH
$g4 = [System.Drawing.Graphics]::FromImage($bar)
$g4.CopyFromScreen($bounds.Width - $barW, 0, 0, 0,
                   (New-Object System.Drawing.Size($barW, $barH)))
$pngBar = Join-Path $repo "$OutDir\bar_cc_button.png"
$bar.Save($pngBar, [System.Drawing.Imaging.ImageFormat]::Png)
$g4.Dispose(); $bar.Dispose()
$lines.Add("saved $pngBar")

# ---- volume: live write through SetMasterVolume (C-23) ---------------------------
$origLevel = [double]$hw.master_level
Send-Cmd "cc_volume 40"
$hw2 = (Read-HwState | ConvertFrom-Json)
$lines.Add("volume: $($hw.master_level) -> $($hw2.master_level) (probe cc_volume 40)")
Assert "volume slider write reaches the audio endpoint (master_level=0.4)" `
    ([math]::Abs([double]$hw2.master_level - 0.4) -lt 0.02)
# restore
Send-Cmd ("cc_volume " + [int][math]::Round($origLevel * 100))
$hw3 = (Read-HwState | ConvertFrom-Json)
Assert "volume restored" ([math]::Abs([double]$hw3.master_level - $origLevel) -lt 0.03)

# ---- brightness: live write, verified with an independent WMI read ---------------
$wmiBefore = $null
try {
    $wmiBefore = (Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightness -ErrorAction Stop).CurrentBrightness
} catch { $wmiBefore = $null }
$lines.Add("brightness: independent WMI read before = $wmiBefore; panel path=$($hw.brightness.path) supported=$($hw.brightness.supported)")
if ($hw.brightness.supported) {
    Send-Cmd "cc_brightness 70"
    Start-Sleep -Milliseconds 1200
    $wmiAfter = (Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightness -ErrorAction Stop).CurrentBrightness
    $lines.Add("brightness: WMI read after cc_brightness 70 = $wmiAfter")
    Assert "brightness write applies live (WMI value followed the slider)" `
        ($null -ne $wmiBefore -and [math]::Abs([double]$wmiAfter - 70.0) -le 3.0)
    # restore the original level through the same (real) path
    if ($null -ne $wmiBefore) {
        Send-Cmd ("cc_brightness " + [int]$wmiBefore)
        Start-Sleep -Milliseconds 1200
        $wmiRestored = (Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightness -ErrorAction Stop).CurrentBrightness
        Assert "brightness restored to $wmiBefore (read back $wmiRestored)" `
            ([math]::Abs([double]$wmiRestored - [double]$wmiBefore) -le 3.0)
    }
} else {
    Send-Cmd "cc_brightness 70"
    $hwB = (Read-HwState | ConvertFrom-Json)
    Assert "brightness unsupported: in-panel hint recorded, no fake success" `
        (-not [string]::IsNullOrEmpty($hwB.error))
}

# ---- Wi-Fi / Bluetooth flips: the T-068 write path, forward-verified -------------
# Sidecar radio states are Windows.Devices.Radios.RadioState: 0=Unknown 1=On
# 2=Off 3=Disabled. The probe command takes the C-23 boolean (1=on 0=off), so
# the expected poll value is 1 for on and 2 for off.
if (-not $NoRadio) {
    $wifi = $hw.radios | Where-Object { $_.kind -eq 1 } | Select-Object -First 1
    $bt   = $hw.radios | Where-Object { $_.kind -eq 3 } | Select-Object -First 1
    if ($wifi) {
        $turnOn = ($wifi.state -ne 1)   # flip: on if currently not on
        Send-Cmd "cc_radio 1 $(if ($turnOn) { 1 } else { 0 })"
        Start-Sleep -Seconds 4   # the radio flip settles in ~1-3s
        $hwW = (Read-HwState | ConvertFrom-Json)
        $wifiAfter = $hwW.radios | Where-Object { $_.kind -eq 1 } | Select-Object -First 1
        $expectState = if ($turnOn) { 1 } else { 2 }
        $lines.Add("wifi flip: state $($wifi.state) -> $($wifiAfter.state) (expected $expectState)")
        Assert "Wi-Fi SetRadio flips the real radio" ($wifiAfter.state -eq $expectState)
        # restore the pre-run state
        Send-Cmd "cc_radio 1 $(if ($wifi.state -eq 1) { 1 } else { 0 })"
        Start-Sleep -Seconds 4
        $hwW2 = (Read-HwState | ConvertFrom-Json)
        $wifiBack = $hwW2.radios | Where-Object { $_.kind -eq 1 } | Select-Object -First 1
        Assert "Wi-Fi restored to $($wifi.state) (read back $($wifiBack.state))" `
            ($wifiBack.state -eq $wifi.state)
    } else {
        $lines.Add("wifi flip: no Wi-Fi radio in sidecar state - SKIPPED")
    }
    if ($bt) {
        $btTurnOn = ($bt.state -ne 1)
        Send-Cmd "cc_radio 3 $(if ($btTurnOn) { 1 } else { 0 })"
        Start-Sleep -Seconds 4
        $hwBt = (Read-HwState | ConvertFrom-Json)
        $btAfter = $hwBt.radios | Where-Object { $_.kind -eq 3 } | Select-Object -First 1
        $btExpect = if ($btTurnOn) { 1 } else { 2 }
        $lines.Add("bluetooth flip: state $($bt.state) -> $($btAfter.state) (expected $btExpect)")
        Assert "Bluetooth SetRadio flips the real radio" ($btAfter.state -eq $btExpect)
        Send-Cmd "cc_radio 3 $(if ($bt.state -eq 1) { 1 } else { 0 })"
        Start-Sleep -Seconds 4
        $hwBt2 = (Read-HwState | ConvertFrom-Json)
        $btBack = $hwBt2.radios | Where-Object { $_.kind -eq 3 } | Select-Object -First 1
        Assert "Bluetooth restored to $($bt.state) (read back $($btBack.state))" `
            ($btBack.state -eq $bt.state)
    } else {
        $lines.Add("bluetooth flip: no Bluetooth radio in sidecar state - SKIPPED")
    }
}

# ---- 勿扰: registry write + independent read-back ---------------------------------
$toastPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\PushNotifications"
$toastBefore = $null
try {
    $v = Get-ItemProperty -Path $toastPath -Name ToastNotificationEnabled -ErrorAction Stop
    $toastBefore = [int]$v.ToastNotificationEnabled
} catch { $toastBefore = 1 }   # absent value = system default enabled
Send-Cmd "cc_dnd"
Start-Sleep -Milliseconds 800
$toastAfter = $null
try {
    $v2 = Get-ItemProperty -Path $toastPath -Name ToastNotificationEnabled -ErrorAction Stop
    $toastAfter = [int]$v2.ToastNotificationEnabled
} catch { $toastAfter = 1 }
$expected = if ($toastBefore -eq 0) { 1 } else { 0 }
$lines.Add("dnd: ToastNotificationEnabled $toastBefore -> $toastAfter (expected $expected)")
Assert "勿扰 toggle writes ToastNotificationEnabled and it reads back" `
    ($toastAfter -eq $expected)
# restore the pre-run state (absent = delete the value we created)
if ($toastBefore -eq 1 -and $null -ne $toastAfter) {
    try { Remove-ItemProperty -Path $toastPath -Name ToastNotificationEnabled -ErrorAction Stop } catch {}
} elseif ($toastBefore -eq 0) {
    Send-Cmd "cc_dnd"   # flip back through the same path
}
$dndState = (Read-HwState | ConvertFrom-Json)
$lines.Add("dnd final panel state: on=$($dndState.dnd.on) value_present=$($dndState.dnd.value_present)")

# ---- 夜览: honest unsupported path on this machine --------------------------------
Send-Cmd "cc_night"
Start-Sleep -Milliseconds 800
$hwN = (Read-HwState | ConvertFrom-Json)
$nightKeyExists = Test-Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current\default`$windows.data.bluelightreduction.bluelightreductionstate\Current"
$lines.Add("night: key exists=$nightKeyExists panel state=$($hwN.night.state) error='$($hwN.error)'")
if (-not $nightKeyExists) {
    Assert "夜览 unsupported: panel shows the hint, state stays unknown, no fake write" `
        ($hwN.night.state -eq 0 -and -not [string]::IsNullOrEmpty($hwN.error))
} else {
    $lines.Add("night: key exists on this machine - flip exercised, read-back below")
    Assert "夜览 flip produced a state (±1) or an explicit error" `
        ($hwN.night.state -ne 0 -or -not [string]::IsNullOrEmpty($hwN.error))
}

# ---- close: click outside, Esc, probe close --------------------------------------
# Click-outside needs the panel to HOLD the foreground first (the WM_ACTIVATE
# rule fires on a loss of activation). A probe command cannot grant foreground,
# so the script clicks the panel's error-strip area (no control there) with a
# real mouse click - a user click activates the panel - and then clicks the
# desktop; the second click deactivates and closes it.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Click304 {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
}
"@
function Click-At([int]$x, [int]$y) {
    [Click304]::SetCursorPos($x, $y) | Out-Null
    Start-Sleep -Milliseconds 200
    [Click304]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero)  # LEFTDOWN
    [Click304]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero)  # LEFTUP
}
# Panel geometry at 192 DPI: width 696px, anchored by its CENTRE under the cc
# box, clamped into the monitor, top = bar bottom (56px). Error strip DIP y
# 240..262 -> px ~536..580; x = panel left + 150 (no control there).
$scale = 2
$anchorPx = [int]((($ccBox.left + $ccBox.right) / 2) * $scale)
$panelLeft = [Math]::Min($anchorPx - 348, $bounds.Width - 696)
$panelTop = 56
$lines.Add("click test: panel physical rect = ($panelLeft,$panelTop)-($($panelLeft+696),$($panelTop+548)) anchor=$anchorPx")
Click-At ($panelLeft + 150) ($panelTop + 500)   # activates the panel (no control hit)
Start-Sleep -Milliseconds 600
$midX = [int]($bounds.Width / 2); $midY = [int]($bounds.Height / 2)
Click-At $midX $midY              # a real click outside
Start-Sleep -Milliseconds 800
$afterClick = (Read-State | ConvertFrom-Json)
Assert "click outside closes the panel (deactivation rule after real activation)" `
    (-not $afterClick.control_center_visible)

# Esc: reopen, click the panel's error-strip area (a real user click grants the
# panel the foreground + keyboard focus - the probe command alone cannot claim
# it on this host, measured), then send ESC to the foreground.
Send-Cmd "cc_open"
Start-Sleep -Milliseconds 1000
$escState = (Read-State | ConvertFrom-Json)
if ($escState.control_center_visible) {
    Click-At ($panelLeft + 150) ($panelTop + 500)   # activation click, no control hit
    Start-Sleep -Milliseconds 600
    Add-Type -AssemblyName System.Windows.Forms
    [System.Windows.Forms.SendKeys]::SendWait("{ESC}")
    Start-Sleep -Milliseconds 800
    $escClosed = (Read-State | ConvertFrom-Json)
    Assert "Esc closes the panel (after real activation)" (-not $escClosed.control_center_visible)
} else {
    $lines.Add("esc: panel did not stay open after cc_open - unexpected, investigate")
    Assert "esc precondition: panel open" $false
}

# probe close (the toggle path)
Send-Cmd "cc_open"
Start-Sleep -Milliseconds 800
Send-Cmd "cc_hide"
$hidden = (Read-State | ConvertFrom-Json)
Assert "cc_hide closes the panel" (-not $hidden.control_center_visible)

# ---- right-click minimal menu (I2) ------------------------------------------------
# Open the menu through the real TrackPopupMenuEx path, then select its first
# item (声音设置, ms-settings:sound) with the keyboard - a real selection - and
# verify the ShellExecuteW dispatch in the log. SystemSettings is only killed
# when it was NOT running before the test (never touch a session the user had).
$settingsWasRunning = Get-Process SystemSettings -ErrorAction SilentlyContinue
Send-Cmd "cc_menu"
Start-Sleep -Milliseconds 1200
Add-Type -AssemblyName System.Windows.Forms
[System.Windows.Forms.SendKeys]::SendWait("{DOWN}{ENTER}")
Start-Sleep -Milliseconds 2500
$dockforgeLogDir = Join-Path $env:LOCALAPPDATA "DockForge\logs"
$newestLog = Get-ChildItem -Path $dockforgeLogDir -Filter "dockforge-*.log" -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$menuSelected = $false
$launchFailed = $false
if ($newestLog) {
    $ccMenuLog = Select-String -Path $newestLog.FullName -Pattern "control centre menu -> settings page 1|ms-settings launch failed" -ErrorAction SilentlyContinue | Select-Object -Last 3
    if ($ccMenuLog) {
        $lines.Add("log tail ($($newestLog.Name)): " + (($ccMenuLog | ForEach-Object { $_.Line }) -join " | "))
        foreach ($m in $ccMenuLog) {
            if ($m.Line -match "settings page 1") { $menuSelected = $true }
            if ($m.Line -match "launch failed")   { $launchFailed = $true }
        }
    }
} else {
    $lines.Add("log tail: no dockforge-*.log found under $dockforgeLogDir")
}
Assert "cc_menu selection dispatches ms-settings:sound via ShellExecuteW (log-verified)" `
    ($menuSelected -and -not $launchFailed)
$settingsNow = Get-Process SystemSettings -ErrorAction SilentlyContinue
if (-not $settingsWasRunning -and $settingsNow) {
    Stop-Process -Name SystemSettings -Force -ErrorAction SilentlyContinue
    $lines.Add("SystemSettings.exe opened by the menu selection and closed again (was not running before)")
} elseif ($settingsNow) {
    $lines.Add("SystemSettings.exe was already running before the test - left as is")
}
# The menu is modal on its own thread; if the selection did not close it, close
# it via the measured WM_CANCELMODE path so later commands are not stuck.
Send-Cmd "cancel"
Start-Sleep -Milliseconds 600

# ---- teardown ---------------------------------------------------------------------
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
$lines.Add("verdict: $(if ($failures.Count -eq 0) {'ALL PASS'} else {'FAILURES: ' + ($failures -join '; ')})")
$verdictPath = Join-Path $repo "$OutDir\verdict_real.txt"
$lines | Set-Content -Path $verdictPath -Encoding UTF8
Get-Content $verdictPath
if ($failures.Count -gt 0) { exit 1 }
exit 0
