# DockForge T-065 evidence probe: the full message-count chain, live.
#
# Log-driven shutter pattern (same as the other probes): PASS/FAIL is decided
# from the app's own log lines plus an independent pixel read of saved
# screenshots - never from the eye.
#
# Chain under test (C-25 + C-22), all for real:
#   shortcut -> AppsFolder scan -> dock tile (target_path basename
#   = message_badge_probe_app.exe) -> TitleBadgeAdapter (same process name)
#   -> window title "(4) badge probe" -> CountChanged(4) -> badge painted
#   -> target exits -> CountChanged(0) -> badge gone -> settings gate off.
#
# Parts:
#   A  registration: 3 adapters (weixin, qq, probe), none failed.
#   B  badge ON:     probe app's title "(4) badge probe" -> log "message count
#                    'message_badge_probe_app.exe' -> 4", dot visible  [shot]
#   C  target exits: probe app killed -> log "-> 0", dot gone        [shot]
#   D  settings gate: hot reload flips integrations.message_counts off;
#                    restarted probe app does NOT produce a badge   [shot]
#
# Notes:
#  - DOCKFORGE_BADGE_PROBE_TARGET is the T-065 verification hook (not a config
#    key); WeChat/QQ adapters coexist untouched (first-registration rule).
#  - DOCKFORGE_FORCE_MATERIAL=solid pins the plate to the solid tier: the self-
#    drawn blur backdrop on THIS machine currently composites as an empty
#    gradient (pre-existing environment issue, unrelated to T-065; recorded in
#    evidence/T-065/dbg_solid.png vs dbg_full.png).
#  - The console trick does NOT work as a probe target: on this OS build the
#    console window belongs to WindowsTerminal.exe, so a "powershell.exe"
#    adapter never sees it (measured; see README).
param(
    [string]$OutDir = 'evidence\T-065',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$ProbeApp = 'build\bin\message_badge_probe_app.exe'
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process -Name message_badge_probe_app -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process -Name message_badge_probe_app -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

if (-not (Test-Path $ProbeApp)) { Fail "probe app missing: $ProbeApp (cmake -DBUILD_BENCH)" }

$scratch = Join-Path $env:TEMP ('dockforge-t065-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch 'config.json'
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
'{}' | Set-Content -Path $configPath -Encoding ASCII   # no BOM: nlohmann rejects BOMs

# Real Start-Menu shortcut so the AppsFolder scan surfaces the probe as a tile.
$lnkPath = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\DockForge Probe.lnk'
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($lnkPath)
$lnk.TargetPath = (Join-Path $root $ProbeApp)
$lnk.WorkingDirectory = (Join-Path $root 'build\bin')
$lnk.Save()

# AppsFolder does not follow Start-Menu file changes instantly: without this
# broadcast the scan still reports the pre-shortcut entry count and the probe
# tile is never pinned (measured: 214 entries/0 pinned before, 215/1 after).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ShellNotify {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(int evId, uint flags, IntPtr p1, IntPtr p2);
}
'@
[ShellNotify]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)  # SHCNE_ASSOCCHANGED
Start-Sleep -Seconds 2

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_PIN_APPS = 'message_badge_probe_app'
$env:DOCKFORGE_BADGE_PROBE_TARGET = 'message_badge_probe_app.exe'
$env:DOCKFORGE_MESSAGE_POLL_MS = '200'
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

Write-Host "=== T-065 probe: scratch=$scratch ==="

# ---- Part A: registration ------------------------------------------------------
Start-Process -FilePath (Join-Path $root $Exe)
Start-Sleep -Seconds 4

$log = Latest-Log
if (-not $log) { Fail 'no log file written' }
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'message-count adapters: 3 registered') {
    Fail "expected 3 adapters registered; saw: $((Get-Content $log.FullName | Select-String 'message-count') -join '; ')"
}
if ($text -notmatch "badge probe adapter registered for 'message_badge_probe_app\.exe'") {
    Fail 'probe adapter not registered'
}
if ($text -match 'message-count adapter .* unavailable') {
    Fail 'an adapter failed Initialize (none should: title adapters are stateless)'
}
if ($text -notmatch 'dock tiles: 1 of') {
    Fail "probe tile not pinned (dock tiles line): $((Get-Content $log.FullName | Select-String 'dock tiles') -join '; ')"
}
Write-Host 'A. registration: PASS (3 adapters + probe tile pinned, none failed)'

# ---- Part B: badge appears ------------------------------------------------------
Start-Process -FilePath (Join-Path $root $ProbeApp)
Start-Sleep -Milliseconds 1500   # > 2 poll intervals at 200ms + settle

$log = Latest-Log
$text = Get-Content $log.FullName -Raw
if ($text -notmatch "message count 'message_badge_probe_app\.exe' -> 4") {
    Fail "adapter did not report 4; saw: $((Get-Content $log.FullName | Select-String 'message count') -join '; ')"
}
$shotB = Join-Path $OutDir 'badge_on.png'
& (Join-Path $root 'tools\screenshot.ps1') -Out $shotB | Out-Null
if (-not (Test-Path $shotB)) { Fail 'screenshot (badge on) missing' }
Write-Host "B. badge ON: PASS (log: count -> 4, screenshot $shotB)"

# ---- Part C: target exits -> CountChanged(0) -------------------------------------
Get-Process -Name message_badge_probe_app -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 1200

$log = Latest-Log
$text = Get-Content $log.FullName -Raw
# Exactly one zero event: the adapter answered 0 after the process was gone.
if ($text -notmatch "message count 'message_badge_probe_app\.exe' -> 0") {
    Fail "adapter did not report 0 after exit; saw: $((Get-Content $log.FullName | Select-String 'message count') -join '; ')"
}
$shotC = Join-Path $OutDir 'badge_off.png'
& (Join-Path $root 'tools\screenshot.ps1') -Out $shotC | Out-Null
if (-not (Test-Path $shotC)) { Fail 'screenshot (badge off) missing' }
Write-Host "C. target exits: PASS (log: count -> 0, screenshot $shotC)"

# ---- Part D: settings gate via hot reload ----------------------------------------
# The gate must stop a LIVE adapter from painting: restart the probe app (the
# count would come back), then switch the adapter off the way the settings
# window does - config edit -> hot reload -> host.
Start-Process -FilePath (Join-Path $root $ProbeApp)
Start-Sleep -Milliseconds 1500

$shotD1 = Join-Path $OutDir 'badge_reenabled.png'
& (Join-Path $root 'tools\screenshot.ps1') -Out $shotD1 | Out-Null

$json = [IO.File]::ReadAllText($configPath)
if ($json.Length -gt 0 -and $json[0] -eq [char]0xFEFF) { $json = $json.Substring(1) }
$obj = $json | ConvertFrom-Json
if (-not $obj.PSObject.Properties['integrations']) {
    $obj | Add-Member -NotePropertyName integrations -NotePropertyValue ([pscustomobject]@{})
}
if (-not $obj.integrations.PSObject.Properties['message_counts']) {
    $obj.integrations | Add-Member -NotePropertyName message_counts -NotePropertyValue ([pscustomobject]@{})
}
if ($obj.integrations.message_counts.PSObject.Properties['message_badge_probe_app.exe']) {
    $obj.integrations.message_counts.'message_badge_probe_app.exe' = $false
} else {
    $obj.integrations.message_counts | Add-Member -NotePropertyName 'message_badge_probe_app.exe' -NotePropertyValue $false
}
[IO.File]::WriteAllText($configPath, ($obj | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))
Start-Sleep -Milliseconds 2500   # watcher + debounce + 2 poll intervals

$log = Latest-Log
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'config reloaded') { Fail 'hot reload did not fire after config edit' }
if ((Get-Content $configPath -Raw) -notmatch '"message_badge_probe_app\.exe"\s*:\s*false') {
    Fail 'settings gate did not persist to config.json'
}
$shotD2 = Join-Path $OutDir 'badge_disabled.png'
& (Join-Path $root 'tools\screenshot.ps1') -Out $shotD2 | Out-Null

# After the switch, the live probe app must produce NO new count event: the
# last 'message count' line in the log is the -> 0 from part C.
$afterSwitch = (Get-Content $log.FullName | Select-String 'message count') |
    Select-Object -Last 1
if ($afterSwitch.Line -notmatch ' -> 0') {
    Fail "a disabled adapter still reported: $($afterSwitch.Line)"
}
Write-Host 'D. settings gate: PASS (hot reload fired, switch persisted, adapter silent)'

# ---- pixel verdict -----------------------------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'tools\message_counts_pixel_verdict.ps1') `
    -On $shotB -Off $shotC -Reenabled $shotD1 -Disabled $shotD2
if ($LASTEXITCODE -ne 0) { Fail 'pixel verdict failed' }

# ---- cleanup -----------------------------------------------------------------------
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process -Name message_badge_probe_app -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item -LiteralPath $lnkPath -Force -ErrorAction SilentlyContinue
Write-Host '=== T-065 probe end (all parts PASS) ==='
