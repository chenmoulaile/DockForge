# DockForge T-051 evidence probe: config hot reload end-to-end.
# 1) external edit of the isolated config.json -> reload adopted + ConfigChanged published
# 2) invalid JSON -> reload rejected, app keeps running on the last good config
# The watch/decide/publish chain is production code; only the edit source is a script.
param(
    [string]$OutDir = 'evidence\T-051',
    [string]$Exe = 'build\bin\DockForge.exe'
)
$ErrorActionPreference = 'Stop'
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$TempDir = Join-Path $env:TEMP ('dockforge-t051-' + [Environment]::TickCount)
$TempConfig = Join-Path $TempDir 'config.json'
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
[System.IO.File]::WriteAllText($TempConfig, '{"dock": {"icon_size": 48, "gap": 8}}',
    (New-Object System.Text.UTF8Encoding($false)))

$report = New-Object System.Collections.Generic.List[string]
function Add-Report([string]$Line) { $script:report.Add($Line); Write-Output $Line }

function Get-LatestLog {
    $dir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
    Get-ChildItem $dir -Filter 'dockforge-*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
function Wait-LogMatch([long]$Offset, [string]$Pattern, [int]$TimeoutMs = 8000) {
    $log = Get-LatestLog
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        if ($log) {
            $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
            try {
                $sr = New-Object System.IO.StreamReader($fs)
                $fs.Position = [Math]::Min($Offset, $fs.Length)
                $text = $sr.ReadToEnd(); $sr.Close()
                foreach ($line in ($text -split "`n")) {
                    if ($line -match $Pattern) { $fs.Close(); return $line }
                }
            } finally { if ($fs) { $fs.Close() } }
        }
        Start-Sleep -Milliseconds 60
        $log = Get-LatestLog
    }
    return $null
}

$offset = (Get-Item (Get-LatestLog).FullName).Length
$env:DOCKFORGE_CONFIG_PATH = $TempConfig
$env:DOCKFORGE_LOG_LEVEL = 'debug'
Remove-Item Env:\DOCKFORGE_SETTINGS -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds 3000

# Case 1: external edit -> adopted + published
[System.IO.File]::WriteAllText($TempConfig, '{"dock": {"icon_size": 64, "gap": 8}}',
    (New-Object System.Text.UTF8Encoding($false)))
$adopted = Wait-LogMatch $offset 'config reloaded .* publishing ConfigChanged' 8000
$published = Wait-LogMatch $offset 'config published icon_size=64' 8000
Add-Report "edit_adopted=$([bool]$adopted) published=$([bool]$published)"
if ($adopted) { Add-Report "adopted_line=$($adopted.Trim())" }

# Case 2: invalid JSON -> rejected, app alive, config untouched
[System.IO.File]::WriteAllText($TempConfig, '{"dock": {"icon_size": ',
    (New-Object System.Text.UTF8Encoding($false)))
$rejected = Wait-LogMatch $offset 'config reload rejected' 8000
Start-Sleep -Milliseconds 500
# Capture liveness BEFORE the probe kills the process - HasExited after the kill
# is true by definition and would fail the verdict on a healthy run.
$aliveAfterInvalid = -not $proc.HasExited
Add-Report "invalid_rejected=$([bool]$rejected) alive=$aliveAfterInvalid"

$proc.Kill(); $proc.WaitForExit()
try { Remove-Item -Recurse -Force $TempDir } catch { }

$fail = @()
if (-not $adopted) { $fail += 'external edit never reached a reload' }
if (-not $published) { $fail += 'ConfigChanged was never published after the reload' }
if (-not $rejected) { $fail += 'invalid JSON was never rejected' }
if (-not $aliveAfterInvalid) { $fail += 'the process died during the invalid-JSON case' }
if ($fail.Count -eq 0) { Add-Report 'verdict=PASS' }
else { Add-Report "verdict=FAIL reasons=$($fail -join '; ')" }
$report | Set-Content -Path (Join-Path $OutDir 'config_reload_probe.txt') -Encoding UTF8
if ($fail.Count -gt 0) { exit 1 }
