# ADR-0058 evidence: right-box geometry + control-centre panel open, through the
# bar's own probe channel (the same wiring tools/t304_evidence.ps1 uses).
$ErrorActionPreference = 'Stop'
$repo = 'D:\MacDock'
Set-Location $repo
$cmdFile   = Join-Path $env:TEMP 'df_bar_cmd.txt'
$stateFile = Join-Path $env:TEMP 'df_bar_state.txt'
Remove-Item -Force -ErrorAction SilentlyContinue $cmdFile, $stateFile
Set-Content -Path $cmdFile -Value "" -Encoding ASCII

$env:DOCKFORGE_CONFIG_PATH          = Join-Path $env:TEMP 'df_bar_config.json'
Set-Content -Path $env:DOCKFORGE_CONFIG_PATH -Value '{"finderbar":{"enabled":true}}' -Encoding ASCII
$env:DOCKFORGE_FINDERBAR            = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE   = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FREEZE_BACKDROP      = "1"

Get-Process DockForge -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Start-Process -FilePath 'D:\MacDock\build\bin\DockForge.exe'
Start-Sleep -Seconds 7

$script:token = 0
function Send-Cmd([string]$text) {
    $script:token++
    Set-Content -Path $cmdFile -Value "$text`n#fix-$script:token-$(Get-Random)" -Encoding ASCII
    Start-Sleep -Milliseconds 900
}
function Read-State {
    Send-Cmd "state"
    Get-Content $stateFile -Raw -Encoding UTF8
}

$state = (Read-State | ConvertFrom-Json)
Write-Output ("height_dip=" + $state.height_dip + " right_width_dip=" + $state.right_width_dip)
foreach ($b in $state.right_boxes) {
    Write-Output ("  box type={0} left={1} right={2} icon={3} text='{4}'" -f $b.type, $b.left, $b.right, $b.has_icon, $b.text)
}

# Open the control centre through the click-equivalent probe path, then leave it
# on screen for the screenshot.
Send-Cmd "cc_open"
$openState = (Read-State | ConvertFrom-Json)
Write-Output ("control_center_visible=" + $openState.control_center_visible)
Start-Sleep -Milliseconds 800
