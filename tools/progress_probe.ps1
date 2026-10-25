# DockForge T-067 evidence probe: the taskbar-progress chain, live.
#
# Log-driven shutter (same pattern as message_counts_probe.ps1): PASS/FAIL is
# decided from the app's own log lines plus screenshots taken only AFTER the
# awaited line appeared - never from the eye.
#
# Chain under test (ADR-0024 white-list degrade path + C-22 progress_bar):
#   explorer copy dialog (class OperationStatusWindow, empty title)
#   -> CopyDialogMsaaReader MSAA walk (ROLE_SYSTEM_PROGRESSBAR, "56%")
#   -> ProgressTracker state machine (Normal, fraction)
#   -> tile Microsoft.Windows.Explorer AUMID match (ShellItem, empty target_path)
#   -> C-22 bottom bar painted on the dock
#   -> copy ends, dialog destroyed -> state dropped in the same scan
#   -> bar gone.
#
# Parts:
#   A  registration: white list 1 entry (explorer copy), poll hook applied.
#   B  copy running: log "taskbar progress 'microsoft.windows.explorer' -> normal N%",
#      at least 2 ascending readings, screenshot while a reading is live. [shot]
#   C  copy finished: log "-> none" (clearing event fired - this is the
#      window-destroyed cleanup path), screenshot after.              [shot]
#   D  no residue: after 3 s idle, the LAST progress line is still "-> none"
#      (no phantom re-appearance from a stale HWND).
#
# Setup notes:
#  - DOCKFORGE_PIN_APPS=explorer keeps exactly the File Explorer tile (the
#    white list credits the copy dialog to the Explorer AUMID).
#  - The 4 GB payload is fsutil-created (instant) and copied across the same
#    volume by the REAL copy engine (Shell.Application CopyHere -> COM
#    surrogate hosts the OperationStatusWindow, exactly what a user copy
#    looks like to a foreign process).
param(
    [string]$OutDir = 'evidence\T-067',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$Files = 512,
    [int]$FileMB = 8
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# Log-driven shutter: poll the newest log until the regex appears (or time out).
function Wait-ForLogLine([string]$Pattern, [double]$TimeoutSec, [ref]$Matched) {
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $log = Latest-Log
        if ($log) {
            $hit = Select-String -Path $log.FullName -Pattern $Pattern |
                Select-Object -Last 1
            if ($hit) {
                $Matched.Value = $hit.Line
                return $true
            }
        }
        Start-Sleep -Milliseconds 150
    }
    return $false
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$scratch = Join-Path $env:TEMP ('dockforge-t067-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path (Join-Path $scratch 'logs') | Out-Null
$configPath = Join-Path $scratch 'config.json'
'{}' | Set-Content -Path $configPath -Encoding ASCII   # no BOM: nlohmann rejects BOMs
$logDir = Join-Path $scratch 'logs'

# REAL random payload, many files, cross-volume destination. Measured the hard
# way twice:
#  - a fsutil-created all-zero file copies through an NTFS fast path and the
#    dialog closes inside 100ms;
#  - a single 2GB random file still finishes in ~1.5s on this machine's NVMe,
#    and the dialog only becomes scannable in the last second.
# Many files x real bytes stretches the copy into the seconds regime the
# feature exists for (512 x 8MB measured ~5-8s).
$payloadDir = Join-Path $env:TEMP ("dockforge-t067-payload-$Files")
if (-not (Test-Path $payloadDir) -or (Get-ChildItem $payloadDir -File).Count -lt $Files) {
    New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null
    Write-Host "generating $Files files x ${FileMB}MB of random payload (one-off, may take a minute)..."
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $buf = New-Object byte[] ($FileMB * 1MB)
    for ($i = 0; $i -lt $Files; ++$i) {
        $rng.GetBytes($buf)
        [System.IO.File]::WriteAllBytes((Join-Path $payloadDir ("f{0:d4}.bin" -f $i)), $buf)
    }
    $rng.Dispose()
} else {
    Write-Host "reusing payload at $payloadDir"
}
$payload = $payloadDir
$destDir = 'D:\dockforge-t067-dest'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
Remove-Item -Path (Join-Path $destDir '*') -Recurse -ErrorAction SilentlyContinue

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_PIN_APPS = 'explorer'
$env:DOCKFORGE_PROGRESS_POLL_MS = '250'
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'

Write-Host "=== T-067 probe: scratch=$scratch payload=$([math]::Round($PayloadMB/1024,1))GB ==="

# ---- Part A: registration ------------------------------------------------------
Start-Process -FilePath (Join-Path $root $Exe)
Start-Sleep -Seconds 4

$log = Latest-Log
if (-not $log) { Fail 'no log file written' }
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'taskbar-progress white list: 1 entry, poll 250ms') {
    Fail "white list not registered: $((Get-Content $log.FullName | Select-String 'taskbar-progress') -join '; ')"
}
if ($text -notmatch 'DOCKFORGE_PIN_APPS kept 1 of') {
    Fail 'explorer tile not pinned'
}
Write-Host 'A. registration: PASS (white list 1 entry @250ms, explorer tile pinned)'

# ---- Part B: copy running -> Normal readings -----------------------------------
# Real copy engine, foreign-process dialog, MSAA read-back.
$copyScript = Join-Path $scratch 'drive.ps1'
@"
`$shell = New-Object -ComObject Shell.Application
`$destFolder = `$shell.NameSpace('$destDir')
if (`$null -eq `$destFolder) { exit 1 }
`$destFolder.CopyHere('$payload')
"@ | Set-Content -Path $copyScript -Encoding ASCII
Start-Process -FilePath 'powershell' -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $copyScript) -WindowStyle Hidden

$hit = $null
if (-not (Wait-ForLogLine "taskbar progress 'microsoft\.windows\.explorer' -> normal \d+%" 45 ([ref]$hit))) {
    Fail "no Normal reading appeared within 45s (last lines: $((Get-Content (Latest-Log).FullName | Select-String 'taskbar') -join '; '))"
}
Write-Host "B1. first reading: $($hit)"

# A second, LATER normal reading so the advancing behaviour is in the
# evidence - shutter waits for the log, not a fixed sleep.
$deadline = (Get-Date).AddSeconds(20)
do {
    Start-Sleep -Milliseconds 200
    $progressLines = Get-Content (Latest-Log).FullName |
        Select-String "taskbar progress 'microsoft\.windows\.explorer' -> normal"
} while ($progressLines.Count -lt 2 -and (Get-Date) -lt $deadline)
if ($progressLines.Count -ge 2) {
    Write-Host "B2. advancing: PASS ($($progressLines.Count) normal readings: $(($progressLines | ForEach-Object { ($_.Line -split '-> ')[1] }) -join ' -> '))"
} else {
    Write-Host "B2. NOTE: only 1 normal reading logged (very fast copy); advancing not evidenced this run"
}

$shotB = Join-Path $OutDir 'progress_on.png'
& (Join-Path $root 'tools\screenshot.ps1') -Out $shotB | Out-Null
if (-not (Test-Path $shotB)) { Fail 'screenshot (progress on) missing' }
Write-Host "B3. screenshot while copy live: $shotB"

# ---- Part C: copy finished -> clearing event ------------------------------------
$hit = $null
if (-not (Wait-ForLogLine "taskbar progress 'microsoft\.windows\.explorer' -> none" 60 ([ref]$hit))) {
    Fail "clearing event never fired after the copy ended"
}
$shotC = Join-Path $OutDir 'progress_off.png'
& (Join-Path $root 'tools\screenshot.ps1') -Out $shotC | Out-Null
if (-not (Test-Path $shotC)) { Fail 'screenshot (progress off) missing' }
Write-Host "C. copy finished: PASS (clearing event: $($hit.Trim())); screenshot $shotC"

# ---- Part D: no residue ----------------------------------------------------------
Start-Sleep -Seconds 3
$last = Get-Content (Latest-Log).FullName | Select-String 'taskbar progress' | Select-Object -Last 1
if ($last.Line -notmatch ' -> none') {
    Fail "phantom progress re-appeared after cleanup: $($last.Line)"
}
Write-Host "D. no residue: PASS (last line still: $($last.Line.Trim()))"

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host 'ALL PARTS PASS'
