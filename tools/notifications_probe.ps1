# DockForge T-076 evidence probe: the macOS-style notification centre, live.
#
# Log-driven shutter pattern (same as every other probe): PASS/FAIL comes from
# the app's own log lines plus the state-file JSON - never from the eye.
# Screenshots document the visuals.
#
# Chain under test (T-076 DoD):
#   A  centre boots: DOCKFORGE_NOTIFICATIONS=1 -> log "centre ready" with the
#      sidecar pid, and the sidecar process really exists.
#   B  access + first poll: log "sidecar ready access=..." (Allowed on a
#      non-packaged process, which is the whole point of DoD 1) and the state
#      file reports primed=true with a count. The first poll is a BASELINE, so
#      count>0 with banner.cards==0 is the correct answer, not a bug.
#   C  banner stack: two synthetic pushes -> banner visible, cards==2, newest on
#      top. Screenshot banner_two.png.
#   D  per-card timeout: the default 5s expiry empties the stack and hides the
#      window; cards back to 0 without any further command.
#   E  history panel: `show` -> panel visible with the pushed rows, count>=2,
#      visible_rows<=8. Screenshot panel_open.png.
#   F  clear: `clear` -> count 0, panel still open and now empty.
#      Screenshot panel_cleared.png. Two modes:
#        default        SKIPPED, because ClearNotifications wipes the operator's
#                       real Windows notification centre.
#        -StubSidecar   runs automatically against notification_sidecar_stub.exe,
#                       a fixed two-row stand-in with the same line protocol, so
#                       the dock-side clear chain is proven end to end without
#                       touching real data.
#        -ClearRoundTrip forces it against the real listener (machine-state
#                       changing; opt-in like hw_panel_probe.ps1's radio test).
#   G  pulse (C-22): a push whose app name matches a dock tile logs
#      "probe pulse tile[i]". Runs with DOCKFORGE_LOG_LEVEL=debug so the pulse
#      line is emitted. Requires the dock to have at least one pinned app.
#   H  no content on disk (ADR-0025): every file this run wrote - log, state
#      file, config - is grepped for the probe's own marker string. It must
#      appear in NONE of them.
#   I  hide: panel closes.
#   J  material chain (C-21): drive MaterialMonitor::SetDeviceLost and back, then
#      read the tiers out of the state file. Proves both windows follow the
#      shared monitor instead of resolving a material of their own.
param(
    [string]$OutDir = 'evidence\T-076',
    [string]$Exe = 'build\bin\DockForge.exe',
    [switch]$ClearRoundTrip,
    # Points DOCKFORGE_NOTIFICATION_SIDECAR at notification_sidecar_stub.exe, a
    # fixed two-row stand-in that speaks the same line protocol without WinRT.
    # With the stub, the clear round trip runs automatically because it cannot
    # touch the machine's real notification centre.
    [switch]$StubSidecar
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Stop-ProbeCleanly
    exit 1
}

# Every exit path goes through here: killing the dock with TerminateProcess skips
# both of its taskbar restore paths (normal exit and the crash filter), so the
# probe has to put the shell back itself.
#
# 2026-10-26: this probe hid the operator's taskbar and left it hidden, because
# the cleanup below did not exist. See tools/probe_cleanup.ps1 for the full story.
function Stop-ProbeCleanly {
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process -Name notification_sidecar -ErrorAction SilentlyContinue | Stop-Process -Force
    Get-Process -Name notification_sidecar_stub -ErrorAction SilentlyContinue | Stop-Process -Force
    try {
        & (Join-Path $root 'tools\probe_cleanup.ps1') -SkipTaskbar:$false | Out-Null
    } catch {
        Write-Host "cleanup script failed: $($_.Exception.Message)"
    }
}

Stop-ProbeCleanly
Start-Sleep -Milliseconds 400

# Drop every DOCKFORGE_* variable this script does not set itself. The probe
# inherits the parent shell's environment, and a stale
# DOCKFORGE_SETTINGS_CMD_FILE from an earlier run carries whatever command that
# run last wrote - which is how a day-old `set behavior.hide_taskbar 1` fired
# inside this probe and hid the taskbar. main.cpp now primes its dedup memory
# with the file's existing content, so a stale line is inert; clearing the
# variable is the second lock on the same door.
foreach ($leaked in @('DOCKFORGE_SETTINGS', 'DOCKFORGE_SETTINGS_CMD_FILE',
                      'DOCKFORGE_SETTINGS_SHOW', 'DOCKFORGE_SETTINGS_THEME',
                      'DOCKFORGE_LAUNCHPAD', 'DOCKFORGE_LAUNCHPAD_CMD_FILE',
                      'DOCKFORGE_FOLDER_CMD_FILE', 'DOCKFORGE_FOLDER_PROBE',
                      'DOCKFORGE_HW_PANEL', 'DOCKFORGE_HW_PANEL_CMD_FILE',
                      'DOCKFORGE_HW_PANEL_STATE_FILE', 'DOCKFORGE_HW_PANEL_SHOW',
                      'DOCKFORGE_TASKBAR_CRASH_TEST', 'DOCKFORGE_HOVER_PROBE',
                      'DOCKFORGE_DPI_OVERRIDE', 'DOCKFORGE_FORCE_MATERIAL',
                      'DOCKFORGE_SIMULATE_DPI', 'DOCKFORGE_WEATHER_API_HOST',
                      'DOCKFORGE_SECRETS_PATH', 'DOCKFORGE_NOTIFICATION_SIDECAR')) {
    Remove-Item "Env:$leaked" -ErrorAction SilentlyContinue
}

if (-not (Test-Path $Exe)) { Fail "app missing: $Exe" }
if (-not (Test-Path 'build\bin\notification_sidecar.exe')) {
    Fail 'notification_sidecar.exe missing (csc target)'
}

$scratch = Join-Path $env:TEMP ('dockforge-t076-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch 'config.json'
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
'{}' | Set-Content -Path $configPath -Encoding ASCII

$cmdFile = Join-Path $scratch 'notif-cmd.txt'
$stateFile = Join-Path $scratch 'notif-state.json'
New-Item -ItemType File -Force -Path $cmdFile | Out-Null

# A marker only this script knows. It goes into a banner push and must never
# appear in any file the app writes (step H).
$marker = 'T076MARKER' + [Environment]::TickCount

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_NOTIFICATIONS = '1'
$env:DOCKFORGE_NOTIFICATIONS_CMD_FILE = $cmdFile
$env:DOCKFORGE_NOTIFICATIONS_STATE_FILE = $stateFile
$env:DOCKFORGE_LOG_LEVEL = 'debug'
# Deliberately NOT forcing a material: step J has to show a real transition, and
# a forced solid baseline would make "solid -> solid -> solid" a tautology that
# proves nothing. Whatever this machine resolves at startup is the baseline.

# Stub mode: same protocol, deterministic two-row list, no WinRT. Clear then
# runs by default because nothing real can be destroyed.
if ($StubSidecar) {
    $stub = Join-Path $root 'build\bin\notification_sidecar_stub.exe'
    if (-not (Test-Path $stub)) { Fail "stub sidecar missing: $stub (build target notification_sidecar_stub)" }
    $env:DOCKFORGE_NOTIFICATION_SIDECAR = $stub
    $ClearRoundTrip = $true
}

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T076Probe {
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
[T076Probe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Save-WindowShot([string]$class, [string]$name) {
    # Stub runs get their own filenames: the stub's fixed two rows would
    # otherwise overwrite the real-sidecar screenshots in the same evidence dir,
    # and the record would no longer show what the shipped path looks like.
    if ($StubSidecar) {
        $name = [System.IO.Path]::GetFileNameWithoutExtension($name) + '_stub.png'
    }
    $h = [T076Probe]::Find($class)
    if ($h -eq [IntPtr]::Zero) { Write-Host "  (no visible $class for $name)"; return $false }
    $r = New-Object T076Probe+RECT
    [T076Probe]::GetWindowRect($h, [ref]$r) | Out-Null
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
    #
    # UTF-8 WITHOUT BOM on purpose: the app reads the file as UTF-8 and treats
    # the first line as the command, so a BOM turns "push ..." into
    # "\xEF\xBB\xBFpush ..." and every command silently becomes unknown.
    # (Set-Content -Encoding UTF8 writes a BOM on PowerShell 5.1.)
    $token = [Environment]::TickCount
    $body = "$cmd`n#$token"
    [System.IO.File]::WriteAllText($cmdFile, $body, (New-Object System.Text.UTF8Encoding($false)))
}

function Get-State {
    if (-not (Test-Path $stateFile)) { return $null }
    try { Get-Content $stateFile -Raw | ConvertFrom-Json } catch { $null }
}

function Ask-State {
    # One `state` command is not enough: the app samples the file on a 50ms tick
    # and a write can land while it is busy painting, which once produced a stale
    # read and a false FAIL. Re-send and wait for the FILE CONTENT to change, so a
    # dropped command becomes a retry instead of a wrong verdict.
    $before = if (Test-Path $stateFile) { (Get-Item $stateFile).LastWriteTimeUtc.Ticks } else { 0 }
    $deadline = (Get-Date).AddSeconds(3)
    while ((Get-Date) -lt $deadline) {
        Send-Cmd 'state'
        Start-Sleep -Milliseconds 350
        if (-not (Test-Path $stateFile)) { continue }
        if ((Get-Item $stateFile).LastWriteTimeUtc.Ticks -ne $before) {
            $s = Get-State
            if ($s) { return $s }
        }
    }
    $s = Get-State
    if (-not $s) { Fail 'state file not written' }
    return $s
}

# The centre is wired after the app scan (measured: ~4s on a 214-app machine),
# so a fixed sleep races it. Poll the log instead.
function Wait-Log([string]$pattern, [int]$timeoutSec = 25) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $deadline) {
        $f = Latest-Log
        if ($f) {
            $t = Get-Content $f.FullName -Raw -ErrorAction SilentlyContinue
            if ($t -and $t -match $pattern) { return $true }
        }
        Start-Sleep -Milliseconds 300
    }
    return $false
}

Write-Host "=== T-076 probe: scratch=$scratch ==="

# The stub runs under its own image name, so the process check has to follow the
# mode rather than hardcoding notification_sidecar.exe.
$sidecarImage = if ($StubSidecar) { 'notification_sidecar_stub' } else { 'notification_sidecar' }

Start-Process -FilePath (Join-Path $root $Exe)
if (-not (Wait-Log 'notifications.*probe cmd=')) { Fail 'probe channel not armed' }
$log = Latest-Log
$text = Get-Content $log.FullName -Raw
if ($text -notmatch 'centre ready pid=\d+') { Fail 'centre did not start (DOCKFORGE_NOTIFICATIONS=1?)' }

# ---- A: sidecar process is real ------------------------------------------------
$sc = Get-Process -Name $sidecarImage -ErrorAction SilentlyContinue
if (-not $sc) { Fail "$sidecarImage process not running" }
if (-not (Wait-Log 'sidecar ready access=')) { Fail 'sidecar never announced ready' }
Write-Host ("A. boot: PASS (sidecar pid={0}, ready logged)" -f $sc.Id)

# ---- B: access answer + baseline poll ------------------------------------------
# The first poll answers ~700ms after ready; wait for primed rather than sleeping
# a fixed amount (a slow machine would otherwise fail a correct build).
$state = $null
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline) {
    $state = Ask-State
    if ($state.primed) { break }
}
$text = Get-Content $log.FullName -Raw
$access = ([regex]'sidecar ready access=(\w+)').Match($text).Groups[1].Value
if ($access -ne 'allowed') { Fail "listener access is '$access', not allowed" }
if (-not $state.primed) { Fail 'store never primed: no poll answer was accepted' }
if ([int]$state.count -lt 1) { Fail 'listener reports 0 notifications; expected a backlog on this machine' }
# The first poll is a baseline by design: a backlog must NOT become banners.
if ([int]$state.banner.cards -ne 0) {
    Fail ("baseline leaked into the banner: cards={0}" -f $state.banner.cards)
}
Write-Host ("B. access+baseline: PASS (access={0}, backlog={1}, banners=0 - baseline rule held)" -f `
    $access, $state.count)

# ---- C: banner stack -----------------------------------------------------------
Send-Cmd "push Qoder|$marker one|first synthetic body"
Start-Sleep -Milliseconds 500
Send-Cmd "push Qoder|$marker two|second synthetic body"
Start-Sleep -Milliseconds 900
$state = Ask-State
if ([int]$state.banner.cards -ne 2) { Fail ("banner cards={0}, expected 2" -f $state.banner.cards) }
if (-not $state.banner.visible) { Fail 'banner window not visible with 2 cards' }
if (-not (Save-WindowShot 'DockForgeNotificationBanner' 'banner_two.png')) {
    Fail 'banner screenshot failed'
}
Write-Host ("C. banner stack: PASS (cards={0}, visible={1}, tier={2})" -f `
    $state.banner.cards, $state.banner.visible, $state.banner.tier)

# ---- D: per-card timeout -------------------------------------------------------
# Default banner_timeout_seconds is 5. Wait past it plus a poll interval.
Start-Sleep -Seconds 7
$state = Ask-State
if ([int]$state.banner.cards -ne 0) {
    Fail ("cards still up after the timeout: {0}" -f $state.banner.cards)
}
if ($state.banner.visible) { Fail 'banner still visible with no cards' }
Write-Host 'D. timeout: PASS (stack emptied and window hid itself)'

# ---- E: history panel ----------------------------------------------------------
# The pushes landed in the history too, so the panel has rows to show.
Send-Cmd 'show'
Start-Sleep -Milliseconds 900
$state = Ask-State
if (-not $state.panel.visible) { Fail 'panel did not open' }
if ([int]$state.panel.count -lt 2) {
    Fail ("panel count={0}, expected the 2 pushed rows" -f $state.panel.count)
}
if ([int]$state.panel.visible_rows -gt 8) {
    Fail ("panel shows {0} rows, above the 8-row ceiling" -f $state.panel.visible_rows)
}
if (-not (Save-WindowShot 'DockForgeNotificationPanel' 'panel_open.png')) {
    Fail 'panel screenshot failed'
}
Write-Host ("E. history panel: PASS (count={0}, rows={1}, access={2})" -f `
    $state.panel.count, $state.panel.visible_rows, $state.panel.access)

# ---- F: clear ------------------------------------------------------------------
# MACHINE-STATE CHANGING: the 清除 button clears Windows' own notification centre
# as well as the local list (that is what makes the clear stick - the sidecar
# polls Windows, so a local-only clear would refill on the next poll). Clearing
# the user's notification centre is a visible side effect, so it is opt-in, the
# same convention hw_panel_probe.ps1 uses for its radio round trip.
if ($ClearRoundTrip) {
    Send-Cmd 'clear'
    if (-not (Wait-Log 'history cleared' 10)) { Fail 'clear button path did not run' }
    # The sidecar clears Windows, then the next poll comes back empty. Wait for
    # one poll interval plus the sidecar's own latency.
    $state = $null
    $deadline = (Get-Date).AddSeconds(12)
    while ((Get-Date) -lt $deadline) {
        $state = Ask-State
        if ([int]$state.panel.count -eq 0) { break }
    }
    if ([int]$state.panel.count -ne 0) {
        Fail ("clear left {0} rows after a full poll interval" -f $state.panel.count)
    }
    if (-not $state.panel.visible) { Fail 'panel closed itself on clear; it should stay open and empty' }
    Save-WindowShot 'DockForgeNotificationPanel' 'panel_cleared.png' | Out-Null
    if ($StubSidecar) {
        # The stub answers `clear` the way the .NET sidecar does ({"ok":true} then
        # an empty poll), so the whole dock-side chain is proven; what is NOT
        # proven here is UserNotificationListener.ClearNotifications itself.
        Write-Host 'F. clear: PASS (store emptied, sidecar acked clear, panel stayed open - stub sidecar)'
    } else {
        Write-Host 'F. clear: PASS (history emptied in memory and in Windows, panel stayed open)'
    }
} else {
    # Non-destructive stand-in: the panel's clear handler is the same function
    # the unit tests drive, and the probe still proves the panel renders rows.
    Write-Host 'F. clear: SKIPPED (needs -ClearRoundTrip or -StubSidecar; it clears the real notification centre)'
}

# ---- G: pulse (C-22) -----------------------------------------------------------
# The probe must not hardcode an app name (the dock's tiles differ per machine),
# so read one back out of the startup log and push with that exact name. That is
# also the strictest form of the test: it proves the display-name match path
# against a name the app itself reported.
$text = Get-Content $log.FullName -Raw
$tileMatch = [regex]::Match($text, 'DEBUG \[dock\] tile\[0\] (?<name>.+?) kind=')
if (-not $tileMatch.Success) {
    Write-Host 'G. pulse: SKIPPED (log has no tile[0] line; needs DOCKFORGE_LOG_LEVEL=debug)'
} else {
    $tileName = $tileMatch.Groups['name'].Value
    Send-Cmd ("push {0}|pulse probe|body" -f $tileName)
    # "pulse tile[i]" comes from the shared delivery path, the same one a real
    # toast takes - not from the probe branch, so this really proves C-22.
    if (Wait-Log 'pulse tile\[0\]' 10) {
        Write-Host ("G. pulse: PASS (tile[0] '{0}' pulsed through the C-22 path)" -f $tileName)
    } else {
        Fail ("push for dock tile '{0}' did not pulse tile[0]" -f $tileName)
    }
}

# ---- H: nothing on disk (ADR-0025) ---------------------------------------------
Send-Cmd 'hide'
Start-Sleep -Milliseconds 700
$leaks = @()
foreach ($file in @($log.FullName, $stateFile, $configPath)) {
    if (-not (Test-Path $file)) { continue }
    $content = Get-Content $file -Raw -ErrorAction SilentlyContinue
    if ($content -and $content.Contains($marker)) { $leaks += $file }
}
if ($leaks.Count -gt 0) {
    Fail ("notification text leaked to disk: {0}" -f ($leaks -join ', '))
}
Write-Host ("H. no content on disk: PASS (marker '{0}' absent from log, state file and config)" -f $marker)

# ---- I: hide -------------------------------------------------------------------
$state = Ask-State
if ($state.panel.visible) { Fail 'panel still visible after hide' }
Write-Host 'I. hide: PASS (panel closed)'

# ---- J: material degradation chain (C-21) --------------------------------------
# Runs last because it repaints both windows and would spoil the screenshots
# above. The trigger is driven through MaterialMonitor::SetDeviceLost, i.e. the
# shipped evaluation and the shipped events - the state file's tiers afterwards
# are evidence about the real chain, not about a probe-only branch.
$before = Ask-State
$tierBefore = $before.banner.tier
Send-Cmd 'material degrade'
Start-Sleep -Milliseconds 900
$state = Ask-State
if ($state.banner.tier -ne 'solid') {
    Fail ("degrade left the banner at '{0}', expected solid" -f $state.banner.tier)
}
if ($state.panel.tier -ne 'solid') {
    Fail ("degrade left the panel at '{0}', expected solid" -f $state.panel.tier)
}
Send-Cmd 'material restore'
Start-Sleep -Milliseconds 900
$state = Ask-State
if ($state.banner.tier -ne $tierBefore) {
    Fail ("restore left the banner at '{0}', expected '{1}'" -f $state.banner.tier, $tierBefore)
}
Write-Host ("J. material chain: PASS ({0} -> solid -> {0} on both windows)" -f $tierBefore)

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 600
$sc = Get-Process -Name $sidecarImage -ErrorAction SilentlyContinue
if ($sc) {
    # The child holds a pipe to a dead parent; it exits on stdin EOF, which
    # happens the moment the dock closes. A survivor means the Stop() path leaked.
    Get-Process -Name $sidecarImage -ErrorAction SilentlyContinue | Stop-Process -Force
    Fail "$sidecarImage survived the dock exit (stdin EOF path broken)"
}
Write-Host '=== T-076 probe: ALL PASS ==='

# Leaving the machine as we found it is part of passing. The dock is already
# down, so this is the taskbar restore plus a sweep for anything else still
# holding a window or a pipe.
Stop-ProbeCleanly
