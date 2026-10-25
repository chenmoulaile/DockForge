# DockForge T-052 evidence probe: theme switching through the settings window.
# 1) set appearance.theme dark  -> flush saved, config.json carries the key,
#    the builtin pack resolves (no theme WARN), ConfigChanged published
# 2) set appearance.theme ghost -> the lens WARNs and the dock keeps its look
# 3) back to default -> key restored
# The chain is production: RecordEdit -> debounce -> SaveConfig -> publish -> lens.
param(
    [string]$OutDir = 'evidence\T-052',
    [string]$Exe = 'build\bin\DockForge.exe'
)
$ErrorActionPreference = 'Stop'
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$TempDir = Join-Path $env:TEMP ('dockforge-t052-' + [Environment]::TickCount)
$TempConfig = Join-Path $TempDir 'config.json'
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
$CmdFile = Join-Path $env:TEMP 'df_settings_probe_cmd.txt'
[System.IO.File]::WriteAllText($CmdFile, '', (New-Object System.Text.UTF8Encoding($false)))

$report = New-Object System.Collections.Generic.List[string]
function Add-Report([string]$Line) { $script:report.Add($Line); Write-Output $Line }
function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}
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
$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $CmdFile
$env:DOCKFORGE_CONFIG_PATH = $TempConfig
$env:DOCKFORGE_LOG_LEVEL = 'debug'
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds 3000

function Read-ConfigTheme {
    try {
        $cfg = Get-Content -Raw -Encoding UTF8 $TempConfig | ConvertFrom-Json
        return $cfg.appearance.theme
    } catch { return '<unreadable>' }
}

# 1) switch to dark
Set-Command 'set appearance.theme dark'
$flush = Wait-LogMatch $offset 'settings\] flush applied=1 changed=1 saved=1' 8000
$warned = Wait-LogMatch $offset "theme 'dark' unavailable" 1500
Start-Sleep -Milliseconds 300
$themeOnDisk = Read-ConfigTheme
Add-Report "dark flush=$([bool]$flush) lens_warned=$([bool]$warned) config_theme=$themeOnDisk"

# 2) unknown theme -> lens falls back, app alive
Set-Command 'set appearance.theme ghost-theme'
$ghostWarn = Wait-LogMatch $offset "theme 'ghost-theme' unavailable" 8000
Start-Sleep -Milliseconds 300
$alive = -not $proc.HasExited
Add-Report "ghost warned=$([bool]$ghostWarn) alive=$alive"

# 3) back to default. The flush line pattern matches every flush (the dark and
# ghost switches each produced one), so the shutter here polls the config file
# itself until the value flips - a fixed sleep raced the 300ms debounce and read
# the ghost value (measured).
Set-Command 'set appearance.theme default'
$themeRestored = '<unset>'
$deadline = [Environment]::TickCount + 8000
while ([Environment]::TickCount -lt $deadline) {
    $themeRestored = Read-ConfigTheme
    if ($themeRestored -eq 'default') { break }
    Start-Sleep -Milliseconds 120
}
Add-Report "default config_theme=$themeRestored"

$proc.Kill(); $proc.WaitForExit()
try { Remove-Item -Recurse -Force $TempDir } catch { }

$fail = @()
if (-not $flush) { $fail += 'switching to dark never flushed' }
if ($warned) { $fail += 'the builtin dark pack failed to resolve' }
if ($themeOnDisk -ne 'dark') { $fail += "config theme is '$themeOnDisk', expected 'dark'" }
if (-not $ghostWarn) { $fail += 'an unknown theme never produced the fallback WARN' }
if (-not $alive) { $fail += 'the process died on an unknown theme' }
if ($themeRestored -ne 'default') { $fail += "config theme is '$themeRestored', expected 'default'" }
if ($fail.Count -eq 0) { Add-Report 'verdict=PASS' }
else { Add-Report "verdict=FAIL reasons=$($fail -join '; ')" }
$report | Set-Content -Path (Join-Path $OutDir 'theme_probe.txt') -Encoding UTF8
if ($fail.Count -gt 0) { exit 1 }
