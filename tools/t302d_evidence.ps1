# DockForge T-302d evidence capture: the Finder Bar centre widget zone, session 2
# (weather / media / calendar_brief renderers + config-level add/remove/reorder).
#
# Modes:
#   default   widgets key omitted -> the frozen default table (5 monitoring
#             entries, ADR-0054). The "默认" screenshot.
#   minimal   widgets = [calendar_brief, weather] with weather.enabled=true +
#             city="Beijing" -> the two opt-in widgets render (weather hits the
#             real open-meteo chain; without a configured city it would be the
#             placeholder, which is also a valid photograph but less useful).
#             NOTE: the city stays ASCII on purpose - PowerShell 5.1 reads a
#             BOM-less .ps1 in the ANSI codepage (936 here), so a non-ASCII
#             literal in this file would reach config.json mojibake'd (measured
#             this session: "北京" became three GBK chars and geocode failed).
#             The "精简" screenshot.
#   full      widgets = all eight C-31 types + weather config + the evidence-only
#             DOCKFORGE_WIDGETS_DEMO=media override (a synthetic GSMTC session -
#             no real player has to run for the photograph). The "全量" screenshot.
#   reorder   the DoD's add/remove/reorder chain, live: start with
#             [weather 0, calendar_brief 1], rewrite the file with swapped
#             orders, assert the row flipped without a restart, then rewrite it
#             down to one entry and assert the removal. State JSON before/after
#             is the evidence; a screenshot documents the end state.
#
# Screenshots land in evidence/T-302/ (the DoD names that directory); probe
# state files and the scratch config land in evidence/T-302d/ (the task's own
# evidence directory). PASS/FAIL comes from the state JSON, not the eye.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools/t302d_evidence.ps1 -Mode full
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("default", "minimal", "full", "reorder")]
    [string]$Mode,
    [string]$StateDir = "evidence/T-302d",
    [string]$ShotDir  = "evidence/T-302",
    [string]$Exe = "build/bin/DockForge.exe"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exePath = Join-Path $repo $Exe
if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }

$scratch = Join-Path $repo "evidence\T-302d\scratch"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $repo $StateDir) | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $repo $ShotDir) | Out-Null

$configPath = Join-Path $scratch "config_$Mode.json"
$cmdFile    = Join-Path $scratch "cmd_$Mode.txt"
$stateFile  = Join-Path $scratch "state_$Mode.txt"

# Base config: bar pinned, right region at its default (clock). The widgets key
# is written per mode; weather is only enabled where the mode wants real data.
function Write-Config([string]$WidgetsJson, [bool]$WithWeather) {
    $weatherLine = ""
    if ($WithWeather) {
        $weatherLine = "  `"weather`": {`n" +
                       "    `"enabled`": true,`n" +
                       "    `"provider`": `"open_meteo`",`n" +
                       "    `"city`": `"Beijing`",`n" +
                       "    `"refresh_minutes`": 30`n" +
                       "  },`n"
    }
    $text = "{`n" +
            $weatherLine +
            "  `"finderbar`": {`n" +
            "    `"enabled`": true,`n" +
            "    `"height`": 28,`n" +
            "    `"widgets`": $WidgetsJson,`n" +
            "    `"auto_hide`": false`n" +
            "  }`n" +
            "}`n"
    # UTF-8 without BOM: JSON must not carry one (repo rule; the parser reads raw bytes).
    [System.IO.File]::WriteAllText($configPath, $text, (New-Object System.Text.UTF8Encoding($false)))
}

$allEight = '[{"type":"cpu","order":0},{"type":"memory","order":1},{"type":"disk","order":2},' +
            '{"type":"network","order":3},{"type":"temperature","order":4},' +
            '{"type":"weather","order":5},{"type":"media","order":6},{"type":"calendar_brief","order":7}]'

switch ($Mode) {
    "default" { Write-Config '[{"type":"cpu","order":0},{"type":"memory","order":1},{"type":"disk","order":2},{"type":"network","order":3},{"type":"temperature","order":4}]' $false }
    "minimal" { Write-Config '[{"type":"calendar_brief","order":0},{"type":"weather","order":1}]' $true }
    "full"    { Write-Config $allEight $true }
    "reorder" { Write-Config '[{"type":"weather","order":0},{"type":"calendar_brief","order":1}]' $true }
}

Remove-Item -Force -ErrorAction SilentlyContinue $cmdFile, $stateFile
Set-Content -Path $cmdFile -Value "" -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH          = $configPath
$env:DOCKFORGE_FINDERBAR            = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE   = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL       = "solid"
if ($Mode -eq "full") {
    # Evidence-only: a synthetic media session so the full-table screenshot shows
    # every T-302d widget. The app logs a WARN when the hook is active.
    $env:DOCKFORGE_WIDGETS_DEMO = "media"
} else {
    Remove-Item Env:DOCKFORGE_WIDGETS_DEMO -ErrorAction SilentlyContinue
}

$proc = Start-Process -FilePath $exePath -PassThru
Start-Sleep -Seconds 12   # bar up + weather geocode/fetch window + sampling periods

# The probe gate dedups identical file content, so every "state" carries a unique
# token on a second line (ProbeCommandOf reads the first line only).
$script:cmdToken = 0
function Send-State {
    $script:cmdToken++
    Set-Content -Path $cmdFile -Value "state`n#t302d-$script:cmdToken-$(Get-Random)" -Encoding ASCII
    Start-Sleep -Seconds 2
}
function Read-State {
    # The app writes UTF-8 (foreground names can be Chinese); ANSI mangles those.
    (Get-Content $stateFile -Raw -Encoding UTF8) | ConvertFrom-Json
}

Send-State
if (-not (Test-Path $stateFile)) { throw "state file was not written: $stateFile" }
$state = Read-State

$script:failures = New-Object 'System.Collections.Generic.List[string]'
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  PASS $what" } else { $script:failures.Add($what); Write-Host "  FAIL $what" }
}

$widgets = $state.widgets
$types = @($widgets.boxes | ForEach-Object { $_.type })

# Shared: every widget box lives between the left group and the right region.
function Check-InsideSpan {
    $leftEnd = $state.layout.left_group_width_dip
    $rightStart = $state.width_dip - $state.right_width_dip - 12.0
    $inside = $true
    foreach ($b in @($widgets.boxes)) {
        if ($b.left -lt $leftEnd - 0.5 -or $b.right -gt $rightStart + 0.5) { $inside = $false }
    }
    Check $inside "span: all boxes inside [left_end=$leftEnd, right_start=$rightStart]"
}

switch ($Mode) {
    "default" {
        # The frozen default table still renders the five monitoring widgets -
        # the three new types are opt-in and must NOT appear by default (ADR-0054).
        Check ($types.Count -ge 4) "default: 4-5 monitoring boxes (got $($types.Count))"
        Check ($types -contains "cpu" -and $types -contains "memory" -and
               $types -contains "disk" -and $types -contains "network") "default: monitoring four present"
        Check (-not ($types -contains "weather")) "default: weather NOT in the default table"
        Check (-not ($types -contains "media")) "default: media NOT in the default table"
        Check (-not ($types -contains "calendar_brief")) "default: calendar_brief NOT in the default table"
        Check-InsideSpan
    }
    "minimal" {
        # DoD: the two opt-in types render, configured via finderbar.widgets alone.
        Check ($types.Count -eq 2) "minimal: exactly 2 boxes (got $($types.Count): $($types -join ','))"
        Check ($types[0] -eq "calendar_brief") "minimal: calendar_brief first (order 0)"
        Check ($types[1] -eq "weather") "minimal: weather second (order 1)"
        $cal = $widgets.boxes | Where-Object { $_.type -eq "calendar_brief" }
        Check ($cal.text -match "^\d{1,2}/\d{1,2}$") "minimal: calendar text is month/day ('$($cal.text)')"
        $wx = $widgets.boxes | Where-Object { $_.type -eq "weather" }
        Check ($wx.icon -eq $true) "minimal: weather box carries the glyph flag"
        Check ($null -ne $wx.text -and $wx.text.Length -gt 0) "minimal: weather text present ('$($wx.text)')"
        Check ($widgets.snapshot.weather_valid -eq $true) "minimal: weather snapshot valid (open-meteo answered)"
        Check-InsideSpan
    }
    "full" {
        # DoD: all eight C-31 types configured -> all eight placed (media comes
        # from the demo session; the rest are live).
        Check ($types.Count -eq 8) "full: all 8 boxes placed (got $($types.Count): $($types -join ','))"
        $expected = @("cpu","memory","disk","network","temperature","weather","media","calendar_brief")
        $missing = @($expected | Where-Object { -not ($types -contains $_) })
        Check ($missing.Count -eq 0) "full: every C-31 type present (missing: $($missing -join ','))"
        $media = $widgets.boxes | Where-Object { $_.type -eq "media" }
        $play  = [string][char]0x25B6   # ▶
        $pause = [string][char]0x23F8   # ⏸
        Check ($null -ne $media -and ($media.text.StartsWith($play) -or $media.text.StartsWith($pause))) `
              "full: media text starts with a play-state glyph ('$($media.text)')"
        Check ($widgets.snapshot.media_has_session -eq $true) "full: demo media session visible in the snapshot"
        Check-InsideSpan
    }
    "reorder" {
        # Before: [weather 0, calendar_brief 1] -> weather left of calendar.
        Check ($types.Count -eq 2 -and $types[0] -eq "weather" -and $types[1] -eq "calendar_brief") `
              "reorder: before = [weather, calendar_brief] (got $($types -join ','))"
        Copy-Item $stateFile (Join-Path $repo "$StateDir\reorder_before.json") -Force
        Write-Host "  saved reorder_before.json"

        # DoD "order 字段生效，重载即变": swap the orders on disk, no restart.
        Write-Config '[{"type":"calendar_brief","order":0},{"type":"weather","order":1}]' $true
        Start-Sleep -Seconds 6   # watcher cadence + reload + one sample period
        Send-State
        $state2 = Read-State
        $types2 = @($state2.widgets.boxes | ForEach-Object { $_.type })
        Check ($types2.Count -eq 2 -and $types2[0] -eq "calendar_brief" -and $types2[1] -eq "weather") `
              "reorder: after swap = [calendar_brief, weather] (got $($types2 -join ','))"
        Copy-Item $stateFile (Join-Path $repo "$StateDir\reorder_after_swap.json") -Force
        Write-Host "  saved reorder_after_swap.json"

        # ...and the removal half: down to one entry, live.
        Write-Config '[{"type":"calendar_brief","order":0}]' $true
        Start-Sleep -Seconds 6
        Send-State
        $state3 = Read-State
        $types3 = @($state3.widgets.boxes | ForEach-Object { $_.type })
        Check ($types3.Count -eq 1 -and $types3[0] -eq "calendar_brief") `
              "reorder: removal live -> [calendar_brief] (got $($types3 -join ','))"
        Copy-Item $stateFile (Join-Path $repo "$StateDir\reorder_after_remove.json") -Force
        Write-Host "  saved reorder_after_remove.json"
    }
}

# Screenshot: full desktop, plus a top-strip close-up. Taken AFTER the checks, so
# the reorder mode photographs the chain's end state (its before/after halves are
# documented by the state JSON pair).
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @"
using System.Runtime.InteropServices;
public static class DpiAwareT302d {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[DpiAwareT302d]::SetProcessDPIAware() | Out-Null
$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bmp = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
$png = Join-Path $repo "$ShotDir\widgets_t302d_$Mode.png"
$bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $bmp.Dispose()
Write-Host "saved $png ($($bounds.Width)x$($bounds.Height))"

# Top-strip crop centred on the widget zone.
$cropW = [Math]::Min(1500, $bounds.Width); $cropH = 56
$cropX = [int](($bounds.Width - $cropW) / 2)
$full = [System.Drawing.Image]::FromFile($png)
$crop = New-Object System.Drawing.Bitmap $cropW, $cropH
$g2 = [System.Drawing.Graphics]::FromImage($crop)
$g2.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $cropW, $cropH),
              (New-Object System.Drawing.Rectangle $cropX, 0, $cropW, $cropH),
              [System.Drawing.GraphicsUnit]::Pixel)
$cropPng = Join-Path $repo "$ShotDir\widgets_t302d_${Mode}_strip.png"
$crop.Save($cropPng, [System.Drawing.Imaging.ImageFormat]::Png)
$g2.Dispose(); $crop.Dispose(); $full.Dispose()
Write-Host "saved $cropPng"

# Persist the probe state next to the task's evidence, every mode.
Copy-Item $stateFile (Join-Path $repo "$StateDir\state_$Mode.txt") -Force
Write-Host "saved $StateDir/state_$Mode.txt"

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

if ($script:failures.Count -gt 0) {
    Write-Host ("verdict: FAIL (" + ($script:failures -join "; ") + ")")
    exit 1
}
Write-Host "verdict: $Mode PASS"
