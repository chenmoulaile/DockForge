# T-067 evidence: handle steady-state across repeated progress polls.
#
# The DoD's real bar is 8 hours; this script is the honest short-form probe of
# the same invariant (method per T-016: GetGuiResources GDIOBJECTS/USEROBJECTS,
# settled = MIN of 8 samples taken 250ms apart, so one-shot lazy allocations
# cannot masquerade as leaks). It runs several real copy rounds through the
# live progress chain (dialog -> MSAA walk -> repaint) and asserts the settled
# handle counts did not grow between the first and last round.
#
# The 8-hour soak itself is listed as remaining work in TASKS.md.
param(
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$Rounds = 3,
    [int]$Files = 192,
    [int]$FileMB = 4
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}

$scratch = Join-Path $env:TEMP ('dockforge-t067h-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path (Join-Path $scratch 'logs') | Out-Null
'{}' | Set-Content -Path (Join-Path $scratch 'config.json') -Encoding ASCII

# Payload (reused across rounds by keeping one source dir and clearing dest).
$payloadDir = Join-Path $env:TEMP "dockforge-t067-payload-$Files"
if (-not (Test-Path $payloadDir) -or (Get-ChildItem $payloadDir -File).Count -lt $Files) {
    New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $buf = New-Object byte[] ($FileMB * 1MB)
    for ($i = 0; $i -lt $Files; ++$i) {
        $rng.GetBytes($buf)
        [System.IO.File]::WriteAllBytes((Join-Path $payloadDir ("f{0:d4}.bin" -f $i)), $buf)
    }
    $rng.Dispose()
}
$destDir = 'D:\dockforge-t067-dest'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null

$env:DOCKFORGE_CONFIG_PATH = Join-Path $scratch 'config.json'
$env:DOCKFORGE_LOG_DIR = Join-Path $scratch 'logs'
$env:DOCKFORGE_PIN_APPS = 'explorer'
$env:DOCKFORGE_PROGRESS_POLL_MS = '250'
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GuiRes {
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr h, uint kind);
}
'@

function Get-Settled([int]$pid_) {
    # MIN over 8 samples (T-016 method): transient lazy allocations drop out.
    $proc = Get-Process -Id $pid_
    if (-not $proc) { return $null }
    $gdi = [uint32]::MaxValue; $user = [uint32]::MaxValue
    for ($i = 0; $i -lt 8; ++$i) {
        $g = [GuiRes]::GetGuiResources($proc.Handle, 0)   # GR_GDIOBJECTS
        $u = [GuiRes]::GetGuiResources($proc.Handle, 1)   # GR_USEROBJECTS
        if ($g -lt $gdi) { $gdi = $g }
        if ($u -lt $user) { $user = $u }
        Start-Sleep -Milliseconds 250
    }
    return @{ Gdi = $gdi; User = $user }
}

function Run-CopyRound {
    Remove-Item -Path (Join-Path $destDir '*') -Recurse -ErrorAction SilentlyContinue
    $copyScript = Join-Path $scratch 'drive.ps1'
    @"
`$shell = New-Object -ComObject Shell.Application
`$destFolder = `$shell.NameSpace('$destDir')
if (`$null -eq `$destFolder) { exit 1 }
`$destFolder.CopyHere('$payloadDir')
"@ | Set-Content -Path $copyScript -Encoding ASCII
    Start-Process -FilePath 'powershell' -ArgumentList @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $copyScript) -WindowStyle Hidden
    # Wait for the clear event: one full dialog lifetime through the tracker.
    $deadline = (Get-Date).AddSeconds(60)
    do {
        Start-Sleep -Milliseconds 300
        $log = Get-ChildItem (Join-Path $scratch 'logs') -Filter '*.log' |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        $last = if ($log) { (Get-Content $log.FullName | Select-String 'taskbar progress' |
            Select-Object -Last 1).Line } else { '' }
    } while (-not ($last -match ' -> none') -and (Get-Date) -lt $deadline)
    if (-not ($last -match ' -> none')) { Fail "round did not finish: $last" }
    Start-Sleep -Seconds 2   # let the dialog's HWNDs fully age out
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Start-Process -FilePath (Join-Path $root $Exe)
Start-Sleep -Seconds 4
$proc = Get-Process -Name DockForge | Select-Object -First 1
if (-not $proc) { Fail 'dock did not start' }

# Warm-up: one full copy so first-touch allocations are outside the baseline
# (T-016: GDI 0->1 / USER 6->7 on the first frames is allocation, not leak).
Write-Host "warm-up copy..."
Run-CopyRound
$first = Get-Settled $proc.Id
Write-Host ("baseline settled: GDI={0} USER={1}" -f $first.Gdi, $first.User)

for ($round = 1; $round -le $Rounds; ++$round) {
    Write-Host "round $round/$Rounds copy..."
    Run-CopyRound
}

$last = Get-Settled $proc.Id
Write-Host ("after $Rounds rounds settled: GDI={0} USER={1}" -f $last.Gdi, $last.User)
$dg = $last.Gdi - $first.Gdi; $du = $last.User - $first.User
Write-Host ("delta: GDI {0:+#;-#;0}  USER {1:+#;-#;0}" -f $dg, $du)

$ok = ($dg -le 0 -and $du -le 0)
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
if ($ok) {
    Write-Host "VERDICT: PASS - no handle growth across $Rounds full copy rounds (8h soak remains)"
} else {
    Write-Host 'VERDICT: FAIL - handle counts grew'
    exit 1
}
