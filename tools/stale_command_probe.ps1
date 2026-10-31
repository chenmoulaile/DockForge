# Regression probe for the 2026-10-26 taskbar incident.
#
# The incident: a probe run inherited DOCKFORGE_SETTINGS_CMD_FILE from the
# previous day, the file still held `set behavior.hide_taskbar 1`, and the app
# executed it on startup because its "last command seen" memory starts empty.
# The operator's taskbar disappeared.
#
# This script proves the fix end to end, on the real binary:
#   A. plant a stale command that would be visible if it ran (hide_taskbar 1)
#   B. start DockForge with that file
#   C. assert the stale command did NOT run (no probe-command log line for it,
#      config still says hide_taskbar false, taskbar still visible)
#   D. write a genuinely new command and assert it DOES run - proving the fix
#      did not simply disable the channel
#
# Cleanup is unconditional: this script must leave no process behind and the
# taskbar visible, whatever happens.
param(
    [string]$OutDir = 'evidence\T-076',
    [string]$Exe = 'build\bin\DockForge.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root

function Stop-ProbeCleanly {
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process -Name notification_sidecar -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process -Name notification_sidecar_stub -ErrorAction SilentlyContinue | Stop-Process -Force
    try { & (Join-Path $root 'tools\probe_cleanup.ps1') | Out-Null } catch { }
}

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Stop-ProbeCleanly
    Pop-Location
    exit 1
}

Stop-ProbeCleanly
Start-Sleep -Milliseconds 400

# Drop inherited channel variables so this script's own setup is the only source.
foreach ($leaked in @('DOCKFORGE_SETTINGS', 'DOCKFORGE_SETTINGS_CMD_FILE',
                      'DOCKFORGE_SETTINGS_SHOW', 'DOCKFORGE_LAUNCHPAD',
                      'DOCKFORGE_LAUNCHPAD_CMD_FILE', 'DOCKFORGE_FOLDER_CMD_FILE',
                      'DOCKFORGE_HW_PANEL', 'DOCKFORGE_HW_PANEL_CMD_FILE',
                      'DOCKFORGE_NOTIFICATIONS', 'DOCKFORGE_NOTIFICATIONS_CMD_FILE',
                      'DOCKFORGE_HOVER_PROBE_FILE')) {
    Remove-Item "Env:$leaked" -ErrorAction SilentlyContinue
}

if (-not (Test-Path $Exe)) { Fail "app missing: $Exe" }

$scratch = Join-Path $env:TEMP ('dockforge-stalecmd-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$configPath = Join-Path $scratch 'config.json'
$cmdFile = Join-Path $scratch 'settings-cmd.txt'

# A config that leaves the taskbar alone. If the stale command runs, the app
# writes hide_taskbar=true into this file, which step C reads back.
'{"behavior":{"hide_taskbar":false}}' |
    Set-Content -Path $configPath -Encoding ASCII

# The planted command. This is byte-for-byte what the incident replayed.
[System.IO.File]::WriteAllText($cmdFile, 'set behavior.hide_taskbar 1',
    (New-Object System.Text.UTF8Encoding($false)))

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $cmdFile
$env:DOCKFORGE_LOG_LEVEL = 'debug'

Write-Host "=== stale-command probe: scratch=$scratch ==="
Write-Host 'A. planted stale command: set behavior.hide_taskbar 1'

Start-Process -FilePath (Join-Path $root $Exe) | Out-Null

# Wait for the app to finish startup. The settings channel polls every 50ms, so
# anything that was going to run has run long before this expires.
$deadline = [Environment]::TickCount + 25000
$log = $null
while ([Environment]::TickCount -lt $deadline) {
    Start-Sleep -Milliseconds 250
    $log = Get-ChildItem $logDir -Filter '*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log -and (Get-Content $log.FullName -Raw) -match 'hide_taskbar off \(default\)') { break }
}
if (-not $log) { Fail 'no log written' }

# Give the channel several poll cycles to prove it stays inert.
Start-Sleep -Seconds 2
$text = Get-Content $log.FullName -Raw
Write-Host 'B. app started'

# --- C: the stale command must not have run ------------------------------------
$stale = 'set behavior.hide_taskbar 1'
if ($text -match [regex]::Escape("probe command=$stale")) {
    Fail 'the stale command was executed on startup (the incident is back)'
}
if ($text -match 'hide_taskbar switch -> 1') {
    Fail 'the app switched hide_taskbar on during startup'
}

$configNow = Get-Content $configPath -Raw | ConvertFrom-Json
if ($configNow.behavior.hide_taskbar -eq $true) {
    Fail 'the stale command reached the config file'
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class DfStaleCheck {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string cls, string win);
    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr h);
    public static bool TaskbarVisible() {
        IntPtr h = FindWindowW("Shell_TrayWnd", null);
        return h != IntPtr.Zero && IsWindowVisible(h);
    }
}
'@
if (-not [DfStaleCheck]::TaskbarVisible()) {
    Fail 'the taskbar is hidden - the stale command ran'
}
Write-Host 'C. stale command inert: PASS (not executed, config untouched, taskbar visible)'

# --- D: a genuinely new command still runs --------------------------------------
# Same file, different content. If the gate were simply "ignore this file", this
# would silently do nothing and the channel would be dead.
[System.IO.File]::WriteAllText($cmdFile, 'show',
    (New-Object System.Text.UTF8Encoding($false)))
$seen = $false
$deadline = [Environment]::TickCount + 8000
while ([Environment]::TickCount -lt $deadline) {
    Start-Sleep -Milliseconds 200
    if ((Get-Content $log.FullName -Raw) -match [regex]::Escape('probe command=show')) {
        $seen = $true
        break
    }
}
if (-not $seen) { Fail 'a new command was ignored - the channel is dead, not fixed' }
Write-Host 'D. new command accepted: PASS (channel still live)'

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$dest = Join-Path $OutDir 'probe_stale_command.log'
Copy-Item $log.FullName $dest -Force
Write-Host ("=== stale-command probe: ALL PASS (log {0}) ===" -f $dest)

Stop-ProbeCleanly
Pop-Location
exit 0
