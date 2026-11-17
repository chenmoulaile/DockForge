# T-302 idle-CPU budget measurement (DoD 4): the widgets' sampling must add less
# than 0.3% idle CPU, measured over the two sampling points this package owns:
#
#   idle_off   widgets = []            -> no sampling at all (A/B baseline)
#   idle_on    widgets = default five  -> bar Tick-chain sampler + thermal thread
#   idle_dual  widgets default AND the hardware panel open, whose C-23 sampler
#              runs its own 2s accumulator while visible - both sampling points
#              active at once.
#
# Method is tools/idle_stats.ps1 unchanged (TotalProcessorTime delta over wall
# clock, divided by core count), pointer parked away from the dock/bar.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools/t302_idle_budget.ps1 -Mode idle_on
param(
    [Parameter(Mandatory = $true)][ValidateSet("idle_off", "idle_on", "idle_dual")][string]$Mode,
    [string]$OutDir = "evidence/T-302",
    [int]$Seconds = 60
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$exePath = Join-Path $repo "build\bin\DockForge.exe"
if (-not (Test-Path $exePath)) { throw "exe not found: $exePath" }

$scratch = Join-Path $repo "evidence\T-302\scratch"
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch "config_$Mode.json"
$cmdFile    = Join-Path $scratch "cmd_$Mode.txt"
$stateFile  = Join-Path $scratch "state_$Mode.txt"

$widgetsJson = switch ($Mode) {
    "idle_off" { "[]" }
    default    { $null }   # key omitted -> frozen default table
}
$widgetsLine = if ($null -ne $widgetsJson) { "    `"widgets`": $widgetsJson,`n" } else { "" }
$text = "{`n  `"finderbar`": {`n    `"enabled`": true,`n    `"height`": 28,`n" +
        $widgetsLine + "    `"auto_hide`": false`n  }`n}`n"
[System.IO.File]::WriteAllText($configPath, $text, (New-Object System.Text.UTF8Encoding($false)))

Remove-Item -Force -ErrorAction SilentlyContinue $cmdFile, $stateFile
Set-Content -Path $cmdFile -Value "" -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH          = $configPath
$env:DOCKFORGE_FINDERBAR            = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE   = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL       = "solid"
Remove-Item Env:DOCKFORGE_WIDGETS_DEMO -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_HW_PANEL   -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_HW_PANEL_CMD_FILE   -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_HW_PANEL_STATE_FILE -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_LOG_DIR    -ErrorAction SilentlyContinue

# The dual mode runs BOTH sampling points: the bar's widget host AND the
# hardware panel's own C-23 2s accumulator, kept alive (visible) for the whole
# window via its probe "show" command.
$hwCmdFile = ""
if ($Mode -eq "idle_dual") {
    $env:DOCKFORGE_HW_PANEL              = "1"
    $hwCmdFile = Join-Path $scratch "hw_cmd.txt"
    $env:DOCKFORGE_HW_PANEL_CMD_FILE     = $hwCmdFile
    $env:DOCKFORGE_HW_PANEL_STATE_FILE   = Join-Path $scratch "hw_state.txt"
    Set-Content -Path $hwCmdFile -Value "" -Encoding ASCII
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
$proc = Start-Process -FilePath $exePath -PassThru
Start-Sleep -Seconds 12   # warmup: first samples, thermal thread, sidecar settle

if ($Mode -eq "idle_dual") {
    # Hardware probe gate also dedups content; a single "show" is unique here.
    Set-Content -Path $hwCmdFile -Value "show" -Encoding ASCII
    Start-Sleep -Seconds 3
    if (-not (Test-Path (Join-Path $scratch "hw_state.txt"))) {
        # The panel state file is written on every non-empty command; missing
        # means the panel never opened - refuse to measure the wrong thing.
        throw "hardware panel did not open (no hw state file)"
    }
}

$out = Join-Path $repo "$OutDir\$Mode.txt"
& (Join-Path $PSScriptRoot "idle_stats.ps1") -Out $out -Seconds $Seconds | Out-Null

if ($Mode -eq "idle_dual") { Set-Content -Path $hwCmdFile -Value "hide" -Encoding ASCII }
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Content $out -Encoding UTF8 | Select-Object -First 12
Write-Host "saved $out"
