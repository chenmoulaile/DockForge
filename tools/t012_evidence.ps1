# T-012 evidence: running indicator states.
#
# Four states, each verified by sampling the dock window itself rather than by
# trusting the app's own log:
#
#   idle        nothing running          -> no dots
#   foreground  app launched and in front -> bright dot (opacity 0.95)
#   running     app alive but behind      -> dim dot   (opacity 0.45)
#   exited      app closed                -> dot gone, within 1s
#
# The dot's brightness is the measurement, not just its presence: a dock that only
# ever draws one opacity would look correct in a screenshot and fail the actual
# requirement (front vs back must be distinguishable).

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$HelperExe = 'D:\7-Zip\7zFM.exe',
    [int]$SettleMs = 1500
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -Name T012Native -Namespace W -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
'@

if (-not (Test-Path $OutDir)) { [void](New-Item -ItemType Directory -Path $OutDir -Force) }
$lines = New-Object System.Collections.Generic.List[string]
function Add-Line { param([string]$Text) $lines.Add($Text); Write-Host $Text }

function Stop-Dock {
    Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
}

function Start-Dock {
    $p = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Milliseconds 2200
    return $p
}

function Sample-Dot {
    param([string]$Tag)
    # Park the cursor far from the dock so the row is in its idle geometry. The
    # indicator does not move on hover, but the icons do, and an idle screenshot is
    # the one whose geometry the probe can derive from the window size alone.
    [void][W.T012Native]::SetCursorPos(20, 20)
    Start-Sleep -Milliseconds 700
    $png = Join-Path $OutDir "$Tag.png"
    & (Join-Path $PSScriptRoot 'screenshot.ps1') -Out $png 2>&1 | Out-Null
    $probe = Join-Path $OutDir "$Tag.indicator.txt"
    & (Join-Path $PSScriptRoot 'indicator_probe.ps1') -Image $png -Out $probe 2>&1 | Out-Null

    $rows = @(Get-Content $probe | Where-Object { $_ -match '^tile\[' })
    $withDot = @($rows | Where-Object { $_ -match 'dot=YES' })
    $brightest = 0.0
    $brightTile = -1
    foreach ($r in $rows) {
        if ($r -match 'max_delta=([\d.]+)') {
            $v = [double]$Matches[1]
            if ($v -gt $brightest) {
                $brightest = $v
                if ($r -match 'tile\[(\d+)\]') { $brightTile = [int]$Matches[1] }
            }
        }
    }
    $verdict = (Get-Content $probe | Where-Object { $_ -match '^verdict=' })
    Add-Line "$Tag with_dot=$($withDot.Count) brightest_delta=$([math]::Round($brightest,1)) brightest_tile=$brightTile $verdict"
    return @{ Count = $withDot.Count; Brightest = $brightest; Tile = $brightTile }
}

function Get-ForegroundName {
    $h = [W.T012Native]::GetForegroundWindow()
    Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -eq $h } |
        Select-Object -First 1 -ExpandProperty ProcessName
}

Stop-Dock
Get-Process -Name 7zFM -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

# --- 1. idle -------------------------------------------------------------
$dock = Start-Dock
Add-Line "=== idle ==="
$idle = Sample-Dot -Tag 'state_idle'

# --- 2. foreground -------------------------------------------------------
# Launched by a real dock click, because that is the only path that can bring a
# window forward (evidence/T-011/fg_plain.txt).
Add-Line "=== foreground ==="
& (Join-Path $PSScriptRoot 'click_probe.ps1') -Out (Join-Path $OutDir 'click_launch.txt') `
    -IconIndex 0 -KeepRunning 2>&1 | Out-Null
Start-Sleep -Milliseconds $SettleMs
Add-Line "foreground_now=$(Get-ForegroundName)"
$fg = Sample-Dot -Tag 'state_foreground'

# --- 3. running (behind) -------------------------------------------------
# Bring a second app forward with a real click on its dock tile, which pushes 7-Zip
# into the background without closing it.
Add-Line "=== running (behind) ==="
& (Join-Path $PSScriptRoot 'click_probe.ps1') -Out (Join-Path $OutDir 'click_second.txt') `
    -IconIndex 4 -KeepRunning 2>&1 | Out-Null
Start-Sleep -Milliseconds $SettleMs
Add-Line "foreground_now=$(Get-ForegroundName)"
$bg = Sample-Dot -Tag 'state_running'

# --- 4. exited -----------------------------------------------------------
# Kill the helper and measure how long the dot takes to disappear. The tracker polls
# every 300ms, so the requirement (<= 1s) leaves one interval of slack plus paint.
Add-Line "=== exited ==="
$killAt = Get-Date
Get-Process -Name 7zFM -ErrorAction SilentlyContinue | Stop-Process -Force
$gone = $false
$elapsedMs = 0
for ($i = 0; $i -lt 40; $i++) {
    Start-Sleep -Milliseconds 100
    [void][W.T012Native]::SetCursorPos(20, 20)
    $png = Join-Path $OutDir 'state_exited_probe.png'
    & (Join-Path $PSScriptRoot 'screenshot.ps1') -Out $png 2>&1 | Out-Null
    $probe = Join-Path $OutDir 'state_exited_probe.indicator.txt'
    & (Join-Path $PSScriptRoot 'indicator_probe.ps1') -Image $png -Out $probe 2>&1 | Out-Null
    $hasSeven = @(Get-Content $probe | Where-Object { $_ -match '^tile\[0\].*dot=YES' }).Count
    $elapsedMs = [int]((Get-Date) - $killAt).TotalMilliseconds
    if ($hasSeven -eq 0) { $gone = $true; break }
}
Add-Line "exit_detected=$gone elapsed_ms=$elapsedMs budget_ms=1000"
$exited = Sample-Dot -Tag 'state_exited'

# --- verdict -------------------------------------------------------------
Add-Line "=== verdict ==="
$okIdle = $idle.Count -eq 0
$okFg = $fg.Count -ge 1
$okBg = $bg.Count -ge 1
$okDim = $bg.Brightest -lt ($fg.Brightest * 0.85)
$okGone = $gone -and $elapsedMs -le 1000
Add-Line "idle_has_no_dots=$okIdle"
Add-Line "foreground_has_dot=$okFg"
Add-Line "running_has_dot=$okBg"
Add-Line "foreground_brighter_than_running=$okDim (fg=$([math]::Round($fg.Brightest,1)) bg=$([math]::Round($bg.Brightest,1)))"
Add-Line "exit_within_1s=$okGone"
$allOk = $okIdle -and $okFg -and $okBg -and $okDim -and $okGone
Add-Line "verdict=$(if ($allOk) { 'ALL_PASS' } else { 'FAILED' })"

Stop-Dock
Get-Process -Name 7zFM -ErrorAction SilentlyContinue | Stop-Process -Force

$outPath = Join-Path $OutDir 'indicator_states.txt'
[System.IO.File]::WriteAllText($outPath, ($lines -join "`n") + "`n",
    (New-Object System.Text.UTF8Encoding($false)))
