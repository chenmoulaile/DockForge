# T-016 evidence: handle accounting for the DWM thumbnails across many
# show/hide cycles, against the live dock.
#
# The spike already measured 40 register/unregister rounds with a flat GDI/USER
# object count (evidence/T-015/bench.txt section D). What it could not measure is
# the product path: HidePreview -> ReleaseAll -> Place -> SetSources ->
# PumpPending, repeated. That loop is where a missing DwmUnregisterThumbnail
# would show up, and it only shows up after many cycles, not one.
#
# Reads the app's own log to count show/hide transitions, then samples
# GetGuiResources on the dock process before and after. A flat pair of numbers
# with a nonzero cycle count is the pass condition; a rising count is the leak.
param(
    [string]$OutDir = 'evidence\T-016',
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$Dpi = 192,
    [int]$SettleMs = 3000,
    [int]$Cycles = 40,
    [int]$LongDwellMs = 900,
    [int]$ShortDwellMs = 250,
    # Show/hide pairs run before the baseline is taken, so the one-time allocations
    # inside the first popup paint are not counted as per-cycle growth.
    [int]$WarmupCycles = 2,
    # Restricts the dock to apps whose display name contains this substring, so the
    # hovered tile is guaranteed to have a live window. Without it the default row
    # is mostly not-running apps and every dwell ends in "no window", which never
    # exercises the registration path this script is measuring.
    [string]$PinApps = 'Sublime'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T016Handles {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern int GetGuiResources(IntPtr hProcess, int uiFlags);
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(int access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    // PROCESS_QUERY_LIMITED_INFORMATION. The full access mask is refused for
    // cross-integrity processes on modern Windows and this flag is enough for
    // GetGuiResources.
    public static int GdiObjects(int pid) {
        IntPtr h = OpenProcess(0x1000, false, pid);
        if (h == IntPtr.Zero) return -1;
        int n = GetGuiResources(h, 0); // 0 = GR_GDIOBJECTS
        CloseHandle(h);
        return n;
    }
    public static int UserObjects(int pid) {
        IntPtr h = OpenProcess(0x1000, false, pid);
        if (h == IntPtr.Zero) return -1;
        int n = GetGuiResources(h, 1); // 1 = GR_USEROBJECTS
        CloseHandle(h);
        return n;
    }
}
'@ -ErrorAction SilentlyContinue
[void][T016Handles]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-LatestLog {
    Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Read-LogFrom([long]$offset) {
    $log = Get-LatestLog
    if ($log -eq $null) { return '' }
    $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        if ($offset -gt $fs.Length) { $offset = 0 }
        $fs.Position = $offset
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
        return $sr.ReadToEnd()
    } finally {
        $fs.Close()
    }
}

function Get-LogLength {
    $log = Get-LatestLog
    if ($log -eq $null) { return 0 }
    return $log.Length
}

# $Pid is a read-only automatic variable in PowerShell (the host process id), so the
# parameter must not be called that: assigning to it fails with "Cannot overwrite
# variable Pid because it is read-only or constant".
function Get-DockRect([int]$TargetPid) {
    $found = [IntPtr]::Zero
    $cb = [T016Handles+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T016Handles]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $TargetPid) { return $true }
        $sb = New-Object System.Text.StringBuilder 256
        [void][T016Handles]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:found = $h; return $false }
        return $true
    }
    $script:found = [IntPtr]::Zero
    [void][T016Handles]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:found -eq [IntPtr]::Zero) { return $null }
    $r = New-Object T016Handles+RECT
    [void][T016Handles]::GetWindowRect($script:found, [ref]$r)
    return $r
}

Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
$env:DOCKFORGE_LOG_LEVEL = 'debug'
$env:DOCKFORGE_PREVIEW = '1'
# Same file channel as preview_probe.ps1: the show/hide cycle has to be driven from
# outside, and SetCursorPos loses every race against a human using the mouse.
$probeFile = Join-Path $env:TEMP 'dockforge_hover_probe.txt'
$env:DOCKFORGE_HOVER_PROBE_FILE = $probeFile
if ($PinApps) { $env:DOCKFORGE_PIN_APPS = $PinApps }
else { Remove-Item Env:\DOCKFORGE_PIN_APPS -ErrorAction SilentlyContinue }
if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }

function Set-HoverProbe([int]$Tile) {
    [System.IO.File]::WriteAllText($probeFile, "$Tile",
        (New-Object System.Text.UTF8Encoding($false)))
}

Set-HoverProbe -1
$runOffset = Get-LogLength
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs
$rect = Get-DockRect $proc.Id
if (-not $rect) { throw 'dock window not found' }

# The hovered tile index comes from the app's own drop-grid line, same argument as
# preview_probe.ps1: recomputing the row here would be a second implementation.
$gridLine = $null
$deadline = (Get-Date).AddSeconds(10)
while ((Get-Date) -lt $deadline -and -not $gridLine) {
    $gridLine = Read-LogFrom $runOffset -split "`n" |
        Where-Object { $_ -match '\[dock\] drop grid scale=' } | Select-Object -Last 1
    if (-not $gridLine) { Start-Sleep -Milliseconds 200 }
}
if (-not $gridLine) { throw 'drop grid log line not found' }
$slotCount = [int]($gridLine | Select-String -Pattern 'slots=(\d+)' -AllMatches).Matches[0].Groups[1].Value
$hoveredTile = [int]([Math]::Floor($slotCount / 2))

# Baseline after the app has settled: the first frames allocate the D2D device,
# the icon bitmaps and the plate geometry, and those are one-time costs that must
# not be attributed to the show/hide loop.
#
# A warm-up show/hide pair runs BEFORE the baseline is taken, for the same reason.
# Measured without it: GDI objects went 0 -> 1 and stayed there for both 8 and 30
# cycles, which is a one-time lazy allocation inside the first popup paint (the
# preview window's background brush or its DWM redirection surface), not a
# per-cycle leak - a leak would have grown to 8 and 30. With the warm-up the delta
# is measured on the steady state, which is what the DoD actually asks about.
for ($w = 1; $w -le $WarmupCycles; $w++) {
    Set-HoverProbe $hoveredTile
    Start-Sleep -Milliseconds $LongDwellMs
    Set-HoverProbe -1
    Start-Sleep -Milliseconds ($ShortDwellMs + 150)
}
# GetGuiResources is an instantaneous read, and the moment right after a hide is not
# steady state: a DWM redirection surface or a USER object can still be alive while
# the popup window is being torn down. Measured: a single read 800ms after the last
# hide returned USER=7, while twelve consecutive reads of the idle process all
# returned 6. Sampling repeatedly and taking the minimum removes that transient
# without weakening the leak test - a real leak raises the floor, it does not
# lengthen the decay.
function Get-Settled([int]$TargetPid, [int]$Samples = 8, [int]$IntervalMs = 250) {
    $g = [int]::MaxValue
    $u = [int]::MaxValue
    for ($i = 0; $i -lt $Samples; $i++) {
        $gi = [T016Handles]::GdiObjects($TargetPid)
        $ui = [T016Handles]::UserObjects($TargetPid)
        if ($gi -lt 0 -or $ui -lt 0) { return [pscustomobject]@{ Gdi = -1; User = -1 } }
        if ($gi -lt $g) { $g = $gi }
        if ($ui -lt $u) { $u = $ui }
        Start-Sleep -Milliseconds $IntervalMs
    }
    return [pscustomobject]@{ Gdi = $g; User = $u }
}

Set-HoverProbe -1
Start-Sleep -Milliseconds 800
$base = Get-Settled $proc.Id
$gdiBefore = $base.Gdi
$userBefore = $base.User
$offset = Get-LogLength

# One cycle = hover until the popup shows, then leave so HidePreview runs. Both
# halves matter: showing allocates thumbnails, hiding must release them.
$half = [Math]::Max(1, [int]($Cycles / 2))
for ($i = 1; $i -le $half; $i++) {
    Set-HoverProbe $hoveredTile
    Start-Sleep -Milliseconds $LongDwellMs
    Set-HoverProbe -1
    Start-Sleep -Milliseconds ($ShortDwellMs + 150)
}
Set-HoverProbe -1
Start-Sleep -Milliseconds 800
$mid = Get-Settled $proc.Id
$gdiMid = $mid.Gdi
$userMid = $mid.User

for ($i = 1; $i -le ($Cycles - $half); $i++) {
    Set-HoverProbe $hoveredTile
    Start-Sleep -Milliseconds $LongDwellMs
    Set-HoverProbe -1
    Start-Sleep -Milliseconds ($ShortDwellMs + 150)
}
Set-HoverProbe -1
Start-Sleep -Milliseconds 800
$after = Get-Settled $proc.Id
$gdiAfter = $after.Gdi
$userAfter = $after.User

$tail = Read-LogFrom $offset
$shown = @($tail -split "`n" | Where-Object { $_ -match '\[preview\] shown tile=' }).Count
$hidden = @($tail -split "`n" | Where-Object { $_ -match '\[preview\] hidden' }).Count
$registered = @($tail -split "`n" | Where-Object { $_ -match '\[thumb\] registered' }).Count

$report = New-Object System.Collections.Generic.List[string]
$report.Add("cycles=$Cycles warmup=$WarmupCycles dpi=$Dpi pid=$($proc.Id)")
$report.Add("gdi_objects_before=$gdiBefore mid=$gdiMid after=$gdiAfter")
$report.Add("user_objects_before=$userBefore mid=$userMid after=$userAfter")
$report.Add("log_shown_lines=$shown log_hidden_lines=$hidden log_registered_lines=$registered")
$report.Add('--- verdict ---')
$ok = $true
# The log counters include the warm-up pairs, so they are checked as a lower bound.
$expected = $Cycles + $WarmupCycles
if ($shown -lt $expected) { $report.Add("FAIL: only $shown of $expected dwells produced a popup"); $ok = $false }
if ($hidden -lt $expected) { $report.Add("FAIL: only $hidden of $expected leaves produced a hide (preview may be stuck)"); $ok = $false }
if ($gdiAfter -lt 0 -or $userAfter -lt 0) { $report.Add('FAIL: could not read GetGuiResources (OpenProcess refused)'); $ok = $false }
else {
    # The leak assertion compares two STEADY-STATE samples (mid -> after), not
    # before -> after. The first popup paint allocates a GDI object and a USER object
    # once (measured: GDI 0->1 held flat across both 8 and 30 cycles); counting that
    # as a leak would fail a correct implementation forever. A real per-cycle leak
    # shows up as a nonzero mid -> after delta over $half cycles.
    $gdiGrowth = $gdiAfter - $gdiMid
    $userGrowth = $userAfter - $userMid
    $report.Add("gdi_steady_growth=$gdiGrowth user_steady_growth=$userGrowth over_cycles=$($Cycles - $half)")
    if ($gdiGrowth -ne 0) { $report.Add("FAIL: GDI objects grew by $gdiGrowth in steady state"); $ok = $false }
    if ($userGrowth -ne 0) { $report.Add("FAIL: USER objects grew by $userGrowth in steady state"); $ok = $false }
}
$report.Add("verdict=$(if ($ok) { 'PASS' } else { 'FAIL' })")

$outPath = Join-Path $OutDir 'preview_handles.txt'
[System.IO.File]::WriteAllText($outPath, ($report -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
$report | ForEach-Object { Write-Output $_ }
