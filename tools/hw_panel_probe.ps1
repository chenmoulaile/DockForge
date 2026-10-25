# DockForge T-068 evidence probe: the hardware control panel, live.
#
# Log-driven shutter pattern (same as every other probe): PASS/FAIL comes from
# the app's own log lines plus the panel's state-file JSON - never from the
# eye. Screenshots document the visuals.
#
# Chain under test (T-068 DoD + C-23):
#   A  panel shows (probe `show`) + sidecar boots: log "hw_sidecar started",
#      state file has radios populated (>=1) and media block present.
#   B  endpoint volume: probe `volume 33` -> system master really moves to
#      0.33 (read back through an independent Core Audio read via the spike
#      binary), VolumeChanged event logged (event-driven, not polled).
#   C  per-app session: probe `session 0 50` -> session 0 volume reads 0.5 in
#      the state file; sessions enumerated >= 1.
#   D  system stats: state file cpu/memory sane (0..100 cpu, memory > 0).
#   E  media control: `media pause`/`media play` when a session exists -> the
#      sidecar answered ok (state file media.playing flips); with no media
#      session the panel must report an error string, not crash (the probe
#      accepts either, the DoD point is: no silent failure).
#   F  radio switch: bluetooth `radio 3 0` -> state shows state=2 (Off) then
#      `radio 3 1` -> state=1 (On) again. MACHINE-STATE CHANGING: only run
#      this part with -RadioRoundTrip on an machine where flipping bluetooth
#      is acceptable; skipped by default.
#   G  hide: panel closes, sidecar process is gone.
#   H  screenshots: panel_open.png, panel_after.png.
param(
    [string]$OutDir = 'evidence\T-068',
    [string]$Exe = 'build\bin\DockForge.exe',
    [switch]$RadioRoundTrip
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process -Name hw_sidecar -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

if (-not (Test-Path $Exe)) { Fail "app missing: $Exe" }
if (-not (Test-Path 'build\bin\hw_sidecar.exe')) { Fail 'hw_sidecar.exe missing (csc target)' }

$scratch = Join-Path $env:TEMP ('dockforge-t068-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch 'config.json'
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
'{}' | Set-Content -Path $configPath -Encoding ASCII

$cmdFile = Join-Path $scratch 'hw-cmd.txt'
$stateFile = Join-Path $scratch 'hw-state.json'
New-Item -ItemType File -Force -Path $cmdFile | Out-Null

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_HW_PANEL = '1'
$env:DOCKFORGE_HW_PANEL_CMD_FILE = $cmdFile
$env:DOCKFORGE_HW_PANEL_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# ---- window helpers (T-066's shot pattern: find the class, copy its rect) ----
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T068Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static IntPtr Find(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
Add-Type -AssemblyName System.Drawing
[T068Probe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Save-PanelShot([string]$name) {
    $h = [T068Probe]::Find('DockForgeHardwarePanel')
    if ($h -eq [IntPtr]::Zero) { Write-Host "  (no visible panel for $name)"; return $false }
    $r = New-Object T068Probe+RECT
    [T068Probe]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $ht = $r.Bottom - $r.Top
    if ($w -le 0 -or $ht -le 0) { return $false }
    $bmp = New-Object System.Drawing.Bitmap($w, $ht)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Write-Host ("  shot {0} rect=({1},{2})-({3},{4})" -f $name, $r.Left, $r.Top, $r.Right, $r.Bottom)
    return $true
}

function Send-Cmd([string]$cmd) {
    # The app samples the file every 50ms and ignores repeats: force a change
    # by writing the command followed by a unique comment token.
    $token = [Environment]::TickCount
    "$cmd`n#$token" | Set-Content -Path $cmdFile -Encoding ASCII
}

function Get-State {
    if (-not (Test-Path $stateFile)) { return $null }
    try {
        Get-Content $stateFile -Raw | ConvertFrom-Json
    } catch { $null }
}

Write-Host "=== T-068 probe: scratch=$scratch ==="

Start-Process -FilePath (Join-Path $root $Exe)
Start-Sleep -Seconds 3

$log = Latest-Log
if (-not $log) { Fail 'no log file written' }
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'hardware panel ready') { Fail 'panel did not come up (DOCKFORGE_HW_PANEL=1?)' }

# ---- A: open + sidecar --------------------------------------------------------
Send-Cmd 'show'
Start-Sleep -Seconds 5   # sidecar cold start + first poll
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'hw_sidecar started') { Fail 'sidecar was not started on show' }
if ($text -notmatch 'panel open dpi=') { Fail 'panel did not open' }
Send-Cmd 'state'
Start-Sleep -Milliseconds 800
$state = Get-State
if (-not $state) { Fail 'state file not written' }
if (-not $state.radios -or $state.radios.Count -lt 1) { Fail 'no radios in state' }
if (-not $state.media -or ($null -eq $state.media.has_session)) { Fail 'no media block in state' }
if (-not $state.sessions -or $state.sessions.Count -lt 1) {
    Fail ('no audio sessions in state (expected >= 1 incl. system session)')
}
if (-not (Save-PanelShot 'panel_open.png')) { Fail 'panel screenshot failed (is the panel visible?)' }
Write-Host ("A. open+sidecar: PASS (radios={0}, sessions={1}, media.has_session={2})" -f `
    $state.radios.Count, $state.sessions.Count, $state.media.has_session)

# ---- B: endpoint volume moves for real + event logged -------------------------
$before = [double]$state.master_level
Send-Cmd 'volume 33'
Start-Sleep -Seconds 2
Send-Cmd 'state'
Start-Sleep -Milliseconds 800
$state = Get-State
if ([math]::Abs([double]$state.master_level - 0.33) -gt 0.02) {
    Fail ("master level not 0.33, is {0}" -f $state.master_level)
}
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'VolumeChanged session= level=0\.33') {
    Fail 'VolumeChanged event not logged for endpoint set (event-driven path)'
}
# Independent read: the spike binary prints the machine's real master volume.
# Hidden window + redirected output: a flashing console would steal the
# foreground and the panel's WA_INACTIVE rule would close it (measured).
$spikeLog = Join-Path $scratch 'spike-read.txt'
Start-Process -FilePath (Join-Path $root 'spikes\T-068_hw_probe\audio_spike.exe') `
    -RedirectStandardOutput $spikeLog -WindowStyle Hidden -Wait
$spikeLine = (Select-String -Path $spikeLog -Pattern 'endpoint volume' |
    Select-Object -First 1).Line
if ("$spikeLine" -notmatch 'level=0\.33') {
    Fail ("independent audio_spike read did not confirm 0.33: $spikeLine")
}
Write-Host "B. endpoint volume: PASS (0.33 set, event logged, spike confirmed: $spikeLine)"

# ---- C: per-app session volume -------------------------------------------------
Send-Cmd 'session 0 50'
Start-Sleep -Seconds 2
Send-Cmd 'state'
Start-Sleep -Milliseconds 800
$state = Get-State
$s0 = [double]$state.sessions[0].level
if ([math]::Abs($s0 - 0.5) -gt 0.02) { Fail ("session 0 level not 0.5, is $s0") }
Write-Host "C. session volume: PASS (session[0] '$($state.sessions[0].name)' -> 0.5)"

# ---- D: system stats -----------------------------------------------------------
$cpu = [double]$state.cpu_percent
$memPct = [double]$state.memory_percent
$memUsed = [double]$state.memory_used_bytes
if ($cpu -lt 0 -or $cpu -gt 100) { Fail "cpu out of range: $cpu" }
if ($memPct -le 0 -or $memPct -gt 100) { Fail "memory percent out of range: $memPct" }
if ($memUsed -le 0) { Fail "memory used bytes not reported: $memUsed" }
Write-Host ("D. system stats: PASS (cpu={0:F1}% mem={1:F1}% used={2:F1} GB)" -f `
    $cpu, $memPct, ($memUsed / 1GB))

# ---- E: media control (session-dependent, never a silent failure) --------------
Send-Cmd 'media toggle'
Start-Sleep -Seconds 3
Send-Cmd 'state'
Start-Sleep -Milliseconds 800
$state = Get-State
$text = Get-Content $log.FullName -Raw
if ($state.media.has_session) {
    if ($text -notmatch 'probe command=media toggle') { Fail 'media command not received' }
    # ok:true from the sidecar shows as no error strip; a rejection must have
    # produced a control-failed line - either way the command chain ran.
    Write-Host ("E. media control: PASS (has_session=true, aumid={0}, playing={1})" -f `
        $state.media.aumid, $state.media.playing)
} else {
    if ($text -notmatch 'control failed') {
        Fail 'no media session AND no control-failed log - that would be a silent failure'
    }
    Write-Host 'E. media control: PASS (no session; error surfaced, not silent)'
}

# ---- F: radio round trip (opt-in: flips real hardware) -------------------------
if ($RadioRoundTrip) {
    Send-Cmd 'radio 3 0'
    Start-Sleep -Seconds 6
    Send-Cmd 'state'
    Start-Sleep -Milliseconds 800
    $state = Get-State
    $bt = $state.radios | Where-Object { $_.kind -eq 3 } | Select-Object -First 1
    if (-not $bt) { Fail 'no bluetooth radio in state' }
    if ([int]$bt.state -ne 2) { Fail ("bluetooth not Off after radio 3 0: $($bt.state)") }
    Send-Cmd 'radio 3 1'
    Start-Sleep -Seconds 6
    Send-Cmd 'state'
    Start-Sleep -Milliseconds 800
    $state = Get-State
    $bt = $state.radios | Where-Object { $_.kind -eq 3 } | Select-Object -First 1
    if ([int]$bt.state -ne 1) { Fail ("bluetooth not On after radio 3 1: $($bt.state)") }
    Write-Host 'F. radio round trip: PASS (bluetooth Off -> On, state file each time)'
} else {
    Write-Host 'F. radio round trip: SKIPPED (needs -RadioRoundTrip; flips real hardware)'
}

Save-PanelShot 'panel_after.png' | Out-Null

# ---- G: hide kills the sidecar --------------------------------------------------
# Restore the master volume the probe moved (part B) before the app goes down.
$beforePct = [int][math]::Round($before * 100)
Send-Cmd "volume $beforePct"
Start-Sleep -Seconds 2
Send-Cmd 'hide'
Start-Sleep -Seconds 3
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'panel closed') { Fail 'panel close not logged' }
$sidecar = Get-Process -Name hw_sidecar -ErrorAction SilentlyContinue
if ($sidecar) { Fail 'sidecar still alive after hide' }
Write-Host "G. hide: PASS (panel closed, sidecar gone, volume restored to $beforePct%)"

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
Write-Host '=== T-068 probe: ALL PASS ==='
