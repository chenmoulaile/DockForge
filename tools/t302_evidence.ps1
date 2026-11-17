# DockForge T-302 evidence capture: the Finder Bar centre widget zone.
#
# Modes:
#   default        widgets = frozen default table (5 entries), no demo override.
#                  Real sampling + real WMI state; reload check included.
#   reduced        starts with the default table, then rewrites the config down
#                  to [{cpu}] and asserts the hot reload picks it up live.
#   off            widgets = [] -> no centre row at all (A/B idle baseline).
#   grade_normal / grade_warn / grade_critical
#                  DOCKFORGE_WIDGETS_DEMO=<normal|warn|critical> screenshots the
#                  three colour tiers deterministically (evidence-only override).
#
# Every mode writes the probe state JSON to the evidence dir; PASS/FAIL comes
# from the state file, not the eye. Screenshots document the visuals.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools/t302_evidence.ps1 -Mode default
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("default", "reduced", "off", "grade_normal", "grade_warn", "grade_critical")]
    [string]$Mode,
    [string]$OutDir = "evidence/T-302",
    [string]$Exe = "build/bin/DockForge.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exePath = Join-Path $repo $Exe
if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }

$scratch = Join-Path $repo "evidence\T-302\scratch"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $repo $OutDir) | Out-Null

$configPath = Join-Path $scratch "config_$Mode.json"
$cmdFile    = Join-Path $scratch "cmd.txt"
$stateFile  = Join-Path $scratch "state.txt"

# Base config: bar on, right region at its default (clock), centre table per mode.
# $null (the "default" modes) OMITS the widgets key entirely - the frozen default
# table then comes from the config member's own initialiser, which is exactly what
# a user who never wrote the key gets. Writing a literal "default" would be
# invalid JSON and silently ride the parse-failure fallback instead.
function Write-Config([object]$WidgetsJson) {
    $widgetsLine = ""
    if ($null -ne $WidgetsJson) {
        $widgetsLine = "    `"widgets`": $WidgetsJson,`n"
    }
    $text = "{`n" +
            "  `"finderbar`": {`n" +
            "    `"enabled`": true,`n" +
            "    `"height`": 28,`n" +
            $widgetsLine +
            "    `"auto_hide`": false`n" +
            "  }`n" +
            "}`n"
    # UTF8 (no BOM for JSON) via .NET so the C++ parser sees clean bytes.
    [System.IO.File]::WriteAllText($configPath, $text, (New-Object System.Text.UTF8Encoding($false)))
}

switch -Regex ($Mode) {
    "default"   { Write-Config $null }   # member default: key omitted entirely
    "reduced"   { Write-Config $null }   # starts default, reduced mid-run
    "off"       { Write-Config "[]" }
    "grade_*"   { Write-Config $null }
}

Remove-Item -Force -ErrorAction SilentlyContinue $cmdFile, $stateFile
Set-Content -Path $cmdFile -Value "" -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH          = $configPath
$env:DOCKFORGE_FINDERBAR            = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE   = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL       = "solid"
if ($Mode -like "grade_*") {
    $tier = $Mode -replace "^grade_", ""
    $env:DOCKFORGE_WIDGETS_DEMO = $tier
} else {
    Remove-Item Env:DOCKFORGE_WIDGETS_DEMO -ErrorAction SilentlyContinue
}

$proc = Start-Process -FilePath $exePath -PassThru
Start-Sleep -Seconds 10   # bar up + at least a few sampling periods (2s cadence)

# The probe gate dedups identical file content (a byte-identical rewrite is a
# true repeat), so every command carries a unique token on a SECOND line -
# ProbeCommandOf reads the first line only (probe_file.cpp:34), and "state"
# alone twice in one run would be swallowed, leaving stale JSON on disk.
$script:cmdToken = 0
function Send-State {
    $script:cmdToken++
    Set-Content -Path $cmdFile -Value "state`n#token-$script:cmdToken-$(Get-Random)" -Encoding ASCII
    Start-Sleep -Seconds 2
}

Send-State
if (-not (Test-Path $stateFile)) { throw "state file was not written: $stateFile" }
# The app writes UTF-8 (foreground names can be Chinese); reading with the
# default ANSI codepage mangles those bytes and breaks the JSON parser.
$stateRaw = Get-Content $stateFile -Raw -Encoding UTF8
$state = $stateRaw | ConvertFrom-Json

$script:failures = New-Object 'System.Collections.Generic.List[string]'
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  PASS $what" } else { $script:failures.Add($what); Write-Host "  FAIL $what" }
}

$widgets = $state.widgets
$types = @($widgets.boxes | ForEach-Object { $_.type })

if ($Mode -eq "off") {
    # DoD (via C-01): an explicitly emptied table renders nothing and must not crash.
    Check (@($widgets.boxes).Count -eq 0) "off: no widget boxes"
    Check ($widgets.placed -eq 0) "off: placed == 0"
    Check ($state.ready -eq $true) "off: bar still up"
}

if ($Mode -eq "default" -or $Mode -eq "reduced") {
    # DoD 1: the five monitoring widgets render in the centre by default. An
    # unavailable temperature sensor drops its own box only (four boxes is the
    # PASS floor; the snapshot JSON records why).
    Check ($types.Count -ge 4) "default: at least 4 of 5 widget boxes placed (got $($types.Count))"
    Check ($types -contains "cpu") "default: cpu present"
    Check ($types -contains "memory") "default: memory present"
    Check ($types -contains "disk") "default: disk present"
    Check ($types -contains "network") "default: network present"
    # DoD 2: the centre row lives BETWEEN the left group and the right region.
    $leftEnd = $state.layout.left_group_width_dip
    $rightStart = $state.width_dip - $state.right_width_dip - 12.0
    $inside = $true
    foreach ($b in @($widgets.boxes)) {
        if ($b.left -lt $leftEnd - 0.5 -or $b.right -gt $rightStart + 0.5) { $inside = $false }
    }
    Check $inside "default: all boxes inside [left_end=$leftEnd, right_start=$rightStart]"
    # Labels are graded per snapshot.
    Check ($null -ne $widgets.snapshot) "default: snapshot JSON present"
    Write-Host ("  snapshot: " + ($widgets.snapshot | ConvertTo-Json -Compress))
}

if ($Mode -eq "reduced") {
    # DoD 6: rewriting finderbar.widgets on disk must hot-reload into the bar.
    Write-Config '[{"type":"cpu","order":0}]'
    Start-Sleep -Seconds 5   # watcher cadence (300ms) + reload + a sample period
    Send-State
    $state2 = (Get-Content $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json)
    $types2 = @($state2.widgets.boxes | ForEach-Object { $_.type })
    Check ($types2.Count -eq 1 -and $types2[0] -eq "cpu") "reduced: hot reload down to [cpu] (got $($types2 -join ','))"

    # ...and back up: rewriting the file WITHOUT the widgets key restores the
    # member's default table without a restart.
    Write-Config $null
    Start-Sleep -Seconds 5
    Send-State
    $state3 = (Get-Content $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json)
    $types3 = @($state3.widgets.boxes | ForEach-Object { $_.type })
    Check ($types3.Count -ge 4) "reduced: hot reload back to the default table (got $($types3.Count))"
    Check ($types3 -contains "temperature" -or -not ($state3.widgets.snapshot.temperature_valid)) `
          "reduced: temperature follows WMI availability"
}

if ($Mode -like "grade_*") {
    # DoD 3: the three tiers colour exactly as frozen. Every percent widget in
    # the row must carry the tier's grade in the state JSON.
    $tier = $Mode -replace "^grade_", ""
    $expected = if ($tier -eq "normal") { "normal" } elseif ($tier -eq "warn") { "warn" } else { "critical" }
    $graded = @($widgets.boxes | Where-Object { $_.type -ne "network" })
    $wrong = @($graded | Where-Object { $_.grade -ne $expected })
    Check ($wrong.Count -eq 0) "grade: every percent widget graded '$expected' (wrong: $($wrong | ForEach-Object { $_.type + '=' + $_.grade }))"
    Check (@($widgets.boxes).Count -ge 5) "grade: temperature visible in demo (5 boxes)"
}

# Screenshot: full desktop plus a top-strip close-up around the centre widgets.
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class DpiAwareT302 {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[DpiAwareT302]::SetProcessDPIAware() | Out-Null
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$png = Join-Path $repo "$OutDir\widgets_$Mode.png"
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $png ($($bounds.Width)x$($bounds.Height))"

# Top-strip crop, centre half: the widgets zone with some margin either side.
$cropW = [Math]::Min(1400, $bounds.Width); $cropH = 56
$cropX = [int](($bounds.Width - $cropW) / 2)
$crop = $bmp2 = $null
$full = [System.Drawing.Image]::FromFile($png)
$crop = New-Object System.Drawing.Bitmap $cropW, $cropH
$g2 = [System.Drawing.Graphics]::FromImage($crop)
$g2.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $cropW, $cropH),
              (New-Object System.Drawing.Rectangle $cropX, 0, $cropW, $cropH),
              [System.Drawing.GraphicsUnit]::Pixel)
$cropPng = Join-Path $repo "$OutDir\widgets_${Mode}_strip.png"
$crop.Save($cropPng, [System.Drawing.Imaging.ImageFormat]::Png)
$g2.Dispose(); $crop.Dispose(); $full.Dispose()
Write-Host "saved $cropPng"

Copy-Item $stateFile (Join-Path $repo "$OutDir\state_$Mode.txt") -Force
Write-Host "saved state_$Mode.txt"

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

if ($script:failures.Count -gt 0) {
    Write-Host ("verdict: FAIL (" + ($script:failures -join "; ") + ")")
    exit 1
}
Write-Host "verdict: $Mode PASS"
