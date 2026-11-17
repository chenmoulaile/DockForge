# T-306 evidence capture: run the built app with the DEFAULT finderbar.right table
# (ADR-0056: clock=0, network=4, battery=5, ime=6), drive the status probes, record
# the 1Hz sampling chain and screenshot the right-corner strip for each status state.
#
# Modes (one run each, app restarted between them):
#   real      - no demo hook: real IME/battery/network state, sampling record,
#               ime toggle via probe, status popup open/close via probe
#   low       - DOCKFORGE_STATUS_DEMO=low: battery 15% (the <20% red state)
#   nobattery - DOCKFORGE_STATUS_DEMO=nobattery: battery item hidden entirely
#   battery   - DOCKFORGE_STATUS_DEMO=battery: battery 87% charging (bolt glyph)
#   wifi4     - DOCKFORGE_STATUS_DEMO=wifi4: Wi-Fi quality 90 (4 lit arcs)
#   wifi2     - DOCKFORGE_STATUS_DEMO=wifi2: Wi-Fi quality 60 (2 lit arcs)
#   offline   - DOCKFORGE_STATUS_DEMO=offline: not connected
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools/t306_evidence.ps1 -Mode real
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("real","low","nobattery","battery","wifi4","wifi2","offline")]
    [string]$Mode,
    [string]$OutDir = "evidence/T-306"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $repo "build\bin\DockForge.exe"
if (-not (Test-Path $exe)) { throw "exe not found: $exe" }

$scratch = Join-Path $repo "evidence\T-306\scratch"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $repo $OutDir) | Out-Null

# Scratch config: finderbar enabled with NO explicit right table - the parse falls
# back to DefaultFinderBarRightItems(), which proves the ADR-0056 default is the
# one the bar actually renders (the state JSON's right_boxes must show all four).
$configPath = Join-Path $scratch "config.json"
$cmdFile    = Join-Path $scratch "cmd.txt"
$stateFile  = Join-Path $scratch "state.txt"
@'
{
  "finderbar": { "enabled": true, "height": 28 }
}
'@ | Set-Content -Path $configPath -Encoding UTF8

Remove-Item -Force -ErrorAction SilentlyContinue $cmdFile, $stateFile
Set-Content -Path $cmdFile -Value "" -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH          = $configPath
$env:DOCKFORGE_FINDERBAR            = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE   = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL       = "solid"
if ($Mode -eq "real") {
    Remove-Item Env:DOCKFORGE_STATUS_DEMO -ErrorAction SilentlyContinue
} else {
    $env:DOCKFORGE_STATUS_DEMO = $Mode
}

# Kill any previous evidence instance (one bar per machine).
Get-Process DockForge -ErrorAction SilentlyContinue | Where-Object {
    $_.Path -eq $exe } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 6   # first sample + first layout

$script:cmdToken = 0
function Send-Cmd([string]$text) {
    # The probe gate dedups byte-identical file content, so every command carries a
    # unique token on a second line (probe_file.h's documented convention) -
    # otherwise a repeated "state" is dropped and the script reads a stale file.
    $script:cmdToken++
    Set-Content -Path $cmdFile -Value "$text`n#t306-$script:cmdToken-$(Get-Random)" -Encoding ASCII
    Start-Sleep -Milliseconds 900
}
function Read-State {
    Send-Cmd "state"
    if (-not (Test-Path $stateFile)) { throw "state file was not written: $stateFile" }
    # The state JSON is UTF-8 (C-07); reading it with the ANSI codepage mangles the
    # foreground application's Chinese name and breaks the JSON string.
    Get-Content $stateFile -Raw -Encoding UTF8
}

$state = Read-State
Copy-Item $stateFile (Join-Path $repo "$OutDir\state_$Mode.txt") -Force

# ---- verdict helpers ----------------------------------------------------------
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("T-306 evidence run mode=$Mode date=$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
$lines.Add("machine: $(Get-CimInstance Win32_OperatingSystem | Select-Object -ExpandProperty Caption)")

function Assert([string]$name, [bool]$ok) {
    $lines.Add(("  [{0}] {1}" -f ($(if ($ok) {"PASS"} else {"FAIL"})), $name))
    if (-not $ok) { $script:failures += $name }
}
$failures = New-Object System.Collections.Generic.List[string]

# ---- right region renders the default table ----------------------------------
$stateObj = $state | ConvertFrom-Json
$types = @($stateObj.right_boxes | ForEach-Object { $_.type })
$lines.Add("right_boxes: $($types -join ', ') (total_width_dip=$($stateObj.right_width_dip))")
if ($Mode -eq "nobattery") {
    # The DoD's hidden state: the battery item is ABSENT by design - assert exactly
    # the three remaining types instead of all four.
    Assert "nobattery: battery box gone, ime+network+clock remain" `
        (($types -contains "clock") -and ($types -contains "network") -and `
         ($types -contains "ime") -and (-not ($types -contains "battery")))
} else {
    Assert "default right table renders clock+network+battery+ime (ADR-0056)" `
        (($types -contains "clock") -and ($types -contains "network") -and `
         ($types -contains "battery") -and ($types -contains "ime"))
}
$networkBox = $stateObj.right_boxes | Where-Object { $_.type -eq "network" } | Select-Object -First 1
$batteryBox = $stateObj.right_boxes | Where-Object { $_.type -eq "battery" } | Select-Object -First 1
$imeBox     = $stateObj.right_boxes | Where-Object { $_.type -eq "ime" }     | Select-Object -First 1
$clockBox   = $stateObj.right_boxes | Where-Object { $_.type -eq "clock" }   | Select-Object -First 1
if ($Mode -eq "nobattery") {
    if ($networkBox -and $imeBox -and $clockBox) {
        Assert "left-to-right = ime < network < clock (battery absent by design)" `
            (($imeBox.left -lt $networkBox.left) -and ($networkBox.left -lt $clockBox.left))
    } else {
        Assert "right region boxes exist" $false
    }
} elseif ($networkBox -and $batteryBox -and $imeBox -and $clockBox) {
    # order counts from the right edge: ime(6) leftmost of the group, network(4)
    # rightmost of the status group, clock(0) at the very edge.
    Assert "left-to-right = ime < battery < network < clock (order 6/5/4/0 from right)" `
        (($imeBox.left -lt $batteryBox.left) -and ($batteryBox.left -lt $networkBox.left) -and `
         ($networkBox.left -lt $clockBox.left))
    Assert "battery item carries the 25 DIP icon beside percent text" `
        ([bool]$batteryBox.icon)
} else {
    Assert "right region boxes exist" $false
}

# ---- status sample ------------------------------------------------------------
$st = $stateObj.status
$lines.Add("status: ime(valid=$($st.ime.valid) label=$($st.ime.label) mode=$($st.ime.mode)) " +
           "battery(present=$($st.battery.present) pct=$($st.battery.percent) charging=$($st.battery.charging)) " +
           "network(connected=$($st.network.connected) online=$($st.network.online) kind=$($st.network.kind) bars=$($st.network.bars))")
if ($Mode -ne "real") {
    # Demo runs: the forced state must be what the mode promises.
    switch ($Mode) {
        "low"       { Assert "demo low: battery present at 15% (red band <20)" `
                          ($st.battery.present -and $st.battery.percent -eq 15) }
        "nobattery" { Assert "demo nobattery: battery item not rendered" `
                          (-not $st.battery_item_rendered) }
        "battery"   { Assert "demo battery: 87% charging, item rendered" `
                          ($st.battery.present -and $st.battery.percent -eq 87 -and `
                           $st.battery.charging -and $st.battery_item_rendered) }
        "wifi4"     { Assert "demo wifi4: Wi-Fi connected, 4 bars" `
                          ($st.network.kind -eq "Wi-Fi" -and $st.network.bars -eq 4) }
        "wifi2"     { Assert "demo wifi2: Wi-Fi connected, 2 bars" `
                          ($st.network.kind -eq "Wi-Fi" -and $st.network.bars -eq 2) }
        "offline"   { Assert "demo offline: not connected, no online" `
                          ((-not $st.network.connected) -and (-not $st.network.online)) }
    }
}

# ---- 1Hz sampling record (real mode only; the DoD's 实测) ----------------------
if ($Mode -eq "real") {
    # The rate is measured between the two SNAPSHOT writes, using the sampler's
    # own GetTickCount64 stamp at its last sample (status.last_sample_tick_ms):
    # read a, sleep 5s, read b, then mean spacing = (lastB-lastA)/(delta-1) ms of
    # real time per sample. The earlier naive version diffed the s1 snapshot
    # against a wall clock taken ~1s later (after the read sleeps and JSON parse)
    # and mis-stated the window; the sampler's own stamps are the honest bracket.
    $s1 = $stateObj.status.samples
    $lastA = $stateObj.status.last_sample_tick_ms
    Start-Sleep -Seconds 5
    $state2 = Read-State
    $stateObj2 = $state2 | ConvertFrom-Json
    $s2 = $stateObj2.status.samples
    $lastB = $stateObj2.status.last_sample_tick_ms
    $delta = $s2 - $s1
    $span = $lastB - $lastA
    $spacing = if ($delta -gt 1) { $span / ($delta - 1) } else { 0.0 }
    $lines.Add("sampling: samples $s1 -> $s2 (delta=$delta), last-sample stamp span=${span}ms of real time")
    $lines.Add("sampling: mean spacing between consecutive samples = $([math]::Round($spacing,0))ms -> rate = $([math]::Round(1000.0 / [math]::Max($spacing, 1.0), 3)) Hz")
    # The fixed period is 1000ms; the accumulator resets (not carries) so the real
    # spacing sits at or just above one second. Allow the frame-quantisation slack
    # (950ms = 1.05 Hz) but nothing looser - the DoD's ceiling is 1Hz.
    Assert "refresh <= 1Hz: mean sample spacing $([math]::Round($spacing,0))ms >= 950ms" `
        ($spacing -ge 950.0)
    Copy-Item $stateFile (Join-Path $repo "$OutDir\state_real_5s_later.txt") -Force
    Assert "host running with samples counted" ($s2 -gt 0)
}

# ---- IME toggle (real mode) ----------------------------------------------------
if ($Mode -eq "real") {
    $modeBefore = $st.ime.mode
    $labelBefore = $st.ime.label
    Send-Cmd "ime"
    $after = (Read-State | ConvertFrom-Json).status
    $modeAfter = $after.ime.mode
    $labelAfter = $after.ime.label
    $lines.Add("ime toggle: mode $modeBefore ($labelBefore) -> $modeAfter ($labelAfter)")
    Assert "ime click toggles conversion mode immediately (IMC_SETCONVERSIONMODE)" `
        (($modeBefore -band 1) -ne ($modeAfter -band 1))
    Assert "label follows the native bit" (($labelBefore -ne $labelAfter) -or (-not $after.ime.valid))
    # Restore the original mode so the run leaves the machine as it found it.
    Send-Cmd "ime"
    $restored = (Read-State | ConvertFrom-Json).status
    Assert "second toggle restores the original mode" `
        ((($modeBefore -band 1) -eq ($restored.ime.mode -band 1)) -or (-not $restored.ime.valid))
}

# ---- status popup (real mode) --------------------------------------------------
if ($Mode -eq "real") {
    Send-Cmd "status"
    $openState = (Read-State | ConvertFrom-Json)
    Assert "status popup opens through the probe (click path)" $openState.status_popup_visible
    Send-Cmd "status"
    $closedState = (Read-State | ConvertFrom-Json)
    Assert "second click closes the popup (toggle)" (-not $closedState.status_popup_visible)
}

# ---- screenshot: right corner strip + full bar ---------------------------------
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class DpiAwareT306 {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[DpiAwareT306]::SetProcessDPIAware() | Out-Null
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds

# For the popup screenshots open it again and leave it visible for the shot.
if ($Mode -eq "real") { Send-Cmd "status" }
Start-Sleep -Milliseconds 800

$cropW = 780; $cropH = 320
if ($bounds.Width -lt $cropW) { $cropW = $bounds.Width }
if ($bounds.Height -lt $cropH) { $cropH = $bounds.Height }
$corner = New-Object System.Drawing.Bitmap $cropW, $cropH
$g = [System.Drawing.Graphics]::FromImage($corner)
$g.CopyFromScreen($bounds.Width - $cropW, 0, 0, 0,
                  (New-Object System.Drawing.Size($cropW, $cropH)))
$png = Join-Path $repo "$OutDir\bar_status_$Mode.png"
$corner.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $corner.Dispose()
$lines.Add("saved $png (${cropW}x${cropH})")

# Full-desktop shot for the popup runs (the card hangs below the bar).
if ($Mode -eq "real") {
    $full = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $g2 = [System.Drawing.Graphics]::FromImage($full)
    $g2.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $png2 = Join-Path $repo "$OutDir\popup_open_real.png"
    $full.Save($png2, [System.Drawing.Imaging.ImageFormat]::Png)
    $g2.Dispose(); $full.Dispose()
    $lines.Add("saved $png2")
    Send-Cmd "status"   # close it again (toggle)
}

# ---- teardown ------------------------------------------------------------------
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
$lines.Add("verdict: $(if ($failures.Count -eq 0) {'ALL PASS'} else {'FAILURES: ' + ($failures -join '; ')})")
$verdictPath = Join-Path $repo "$OutDir\verdict_$Mode.txt"
$lines | Set-Content -Path $verdictPath -Encoding UTF8
Get-Content $verdictPath
if ($failures.Count -gt 0) { exit 1 }
exit 0
