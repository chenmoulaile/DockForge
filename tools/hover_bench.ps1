# Frame-budget measurement for the hover-dwell preview (T-016).
#
# hover_bench.ps1 measures the ramp, which says nothing about the popup: the
# preview only appears while the pointer is stationary, and the expensive part
# (DwmRegisterThumbnail) only happens on the frames AFTER the latch fires. This
# script therefore parks the pointer on one tile for the whole window instead of
# sweeping it, so the preview opens once and then fills its slots over the next
# few frames.
#
# Two runs are needed and the difference matters (T-012 lesson: alternating A/B
# is the only way to attribute a 0.017ms signal):
#   -Run with the default (DOCKFORGE_PREVIEW=1): latch fires, popup shows,
#     registrations land one per tick.
#   -Run with DOCKFORGE_PREVIEW=0: everything else identical, no popup at all.
# The two p99 columns side by side are the answer; a single run proves nothing.
#
# Baseline rule carried over from C-15i: FrameClock is a 1000-sample rolling
# window and the startup frame is ~309ms, so the first ~16 seconds of p99 are
# garbage. The app prints "frames=" lines once per second; this script waits for
# frames=1000 before it trusts any of them.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$Dpi = 192,
    [int]$SettleMs = 3000,
    # Long enough that the popup has opened and all slots are registered, plus
    # the 1000-frame baseline window. 20s gives ~18s of trustworthy p99 samples.
    [int]$Seconds = 20,
    # Park the pointer inside one tile, do not move it: motion would cancel the
    # dwell (C-17a) and the popup would never appear.
    [int]$MoveEveryMs = 0,
    # Each configuration runs this many times. The on/off delta is only meaningful
    # relative to the spread between REPEATS of the SAME configuration, and that
    # spread is machine-load dependent (measured 18.19 / 18.80 / 18.87ms for three
    # runs of the same build). A single pair cannot separate signal from noise.
    [int]$Repeats = 2
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class T016Bench {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint p);
    [DllImport("winmm.dll")] public static extern uint timeEndPeriod(uint p);
    [DllImport("kernel32.dll")] public static extern IntPtr CreateWaitableTimerExW(IntPtr a, IntPtr n, uint f, uint m);
    [DllImport("kernel32.dll")] public static extern bool SetWaitableTimer(IntPtr h, ref long due, int period, IntPtr r, IntPtr s, bool resume);
    [DllImport("kernel32.dll")] public static extern uint WaitForSingleObject(IntPtr h, uint ms);

    // Same waitable timer AppLoop uses (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), with
    // nothing behind it. Whatever this reports is the machine's scheduling floor.
    public static double[] SampleTimerJitter(int frames) {
        var res = new double[frames];
        const uint CREATE_WAITABLE_TIMER_HIGH_RESOLUTION = 0x2;
        const uint TIMER_ALL_ACCESS = 0x1F0003;
        IntPtr t = CreateWaitableTimerExW(IntPtr.Zero, IntPtr.Zero,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (t == IntPtr.Zero) return res;
        timeBeginPeriod(1);
        long due = -160000;  // 16ms relative, in 100ns units
        var sw = Stopwatch.StartNew();
        for (int i = 0; i < frames; i++) {
            SetWaitableTimer(t, ref due, 0, IntPtr.Zero, IntPtr.Zero, false);
            WaitForSingleObject(t, 1000);
            res[i] = sw.Elapsed.TotalMilliseconds;
            sw.Restart();
        }
        timeEndPeriod(1);
        return res;
    }
}
'@ -ErrorAction SilentlyContinue
[void][T016Bench]::SetProcessDpiAwarenessContext([IntPtr](-4))

$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'

function Get-LatestLog {
    Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Get-DockRect([int]$TargetPid) {
    $found = [IntPtr]::Zero
    $cb = [T016Bench+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T016Bench]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $TargetPid) { return $true }
        $sb = New-Object System.Text.StringBuilder 256
        [void][T016Bench]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:found = $h; return $false }
        return $true
    }
    $script:found = [IntPtr]::Zero
    [void][T016Bench]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:found -eq [IntPtr]::Zero) { return $null }
    $r = New-Object T016Bench+RECT
    [void][T016Bench]::GetWindowRect($script:found, [ref]$r)
    return $r
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

# Run one configuration end to end and return its parsed frame lines.
function Invoke-Run([bool]$PreviewEnabled, [string]$Tag) {
    Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500

    $env:DOCKFORGE_FORCE_MATERIAL = 'solid'
    $env:DOCKFORGE_LOG_LEVEL = 'debug'
    $env:DOCKFORGE_PREVIEW = if ($PreviewEnabled) { '1' } else { '0' }
    if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }

    # The probe file must be handed to the child at CreateProcess time; setting the
    # variable after Start-Process only mutates this script's environment.
    $probeFile = Join-Path $env:TEMP 'dockforge_hover_probe.txt'
    [System.IO.File]::WriteAllText($probeFile, '-1',
        (New-Object System.Text.UTF8Encoding($false)))
    $env:DOCKFORGE_HOVER_PROBE_FILE = $probeFile

    # Captured before launch so the log slice belongs to this run only; reading from
    # offset 0 picks up the previous run's drop grid line and aims at a tile that may
    # not exist this time.
    $startOffset = (Get-LatestLog).Length

    $proc = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Milliseconds $SettleMs
    $rect = Get-DockRect $proc.Id
    if (-not $rect) { throw 'dock window not found' }

    # Hover comes from the probe file, not from SetCursorPos: the pointer warps lose
    # every race against a human using the mouse (measured in preview_probe.ps1), and
    # a bench whose hover state is not stable measures nothing.
    $gridLine = $null
    $gridDeadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $gridDeadline -and -not $gridLine) {
        $gridLine = Read-LogFrom $startOffset -split "`n" |
            Where-Object { $_ -match '\[dock\] drop grid scale=' } | Select-Object -Last 1
        if (-not $gridLine) { Start-Sleep -Milliseconds 200 }
    }
    $hoveredTile = 0
    if ($gridLine) {
        $slotCount = [int]($gridLine | Select-String -Pattern 'slots=(\d+)' -AllMatches).Matches[0].Groups[1].Value
        $hoveredTile = [int]([Math]::Floor($slotCount / 2))
    }
    [System.IO.File]::WriteAllText($probeFile, "$hoveredTile",
        (New-Object System.Text.UTF8Encoding($false)))

    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
    }

    # Release the hover so the tail sample is not mid-popup.
    [System.IO.File]::WriteAllText($probeFile, '-1',
        (New-Object System.Text.UTF8Encoding($false)))
    Start-Sleep -Milliseconds 400

    $tail = Read-LogFrom $startOffset
    $frameLines = @()
    $previewLines = @()
    foreach ($line in ($tail -split "`n")) {
        if ($line -match 'frames=\d+') { $frameLines += $line.TrimEnd() }
        if ($line -match '\[preview\]') { $previewLines += $line.TrimEnd() }
    }

    # Only windows that have a FULL 1000-sample ring count. FrameClock is a rolling
    # window, so before 1000 frames it still holds the startup stall (first frame
    # measured at 309ms in T-012), and averaging those windows reports the launch cost
    # as if it were steady-state jitter. Measured: windows at frames=63..377 read
    # p99=19.6ms while the same process later reads 16.5ms. This is the same rule as
    # C-15i, applied to a different bench.
    $p99s = @(); $avgs = @()
    foreach ($line in $frameLines) {
        if ($line -notmatch 'frames=(\d+)') { continue }
        if ([int]$matches[1] -lt 1000) { continue }
        if ($line -match 'avg=([0-9.]+)ms') { $avgs += [double]$matches[1] }
        if ($line -match 'p99=([0-9.]+)ms') { $p99s += [double]$matches[1] }
    }

    $summary = [ordered]@{
        tag                = $Tag
        preview_enabled    = $PreviewEnabled
        frame_samples      = $p99s.Count
        avg_mean           = if ($avgs.Count) { [Math]::Round(($avgs | Measure-Object -Average).Average, 3) } else { $null }
        p99_mean           = if ($p99s.Count) { [Math]::Round(($p99s | Measure-Object -Average).Average, 3) } else { $null }
        p99_max            = if ($p99s.Count) { [Math]::Round(($p99s | Measure-Object -Maximum).Maximum, 3) } else { $null }
        over_budget        = if ($p99s.Count) { @($p99s | Where-Object { $_ -gt 16.7 }).Count } else { 0 }
        preview_log_lines  = $previewLines.Count
    }
    return $summary
}

# Third configuration: dock running, popup enabled, but nothing hovered. Establishes
# this machine's floor so an OVER_BUDGET reading on the A/B pair can be attributed.
function Invoke-IdleRun([string]$Tag) {
    Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500

    $env:DOCKFORGE_FORCE_MATERIAL = 'solid'
    $env:DOCKFORGE_LOG_LEVEL = 'debug'
    $env:DOCKFORGE_PREVIEW = '1'
    Remove-Item Env:\DOCKFORGE_HOVER_PROBE_FILE -ErrorAction SilentlyContinue
    if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }

    $startOffset = (Get-LatestLog).Length
    $proc = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Milliseconds $SettleMs
    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 250 }

    $tail = Read-LogFrom $startOffset
    $p99s = @(); $avgs = @()
    foreach ($line in ($tail -split "`n")) {
        if ($line -notmatch 'frames=\d+') { continue }
        if ($line -match 'avg=([0-9.]+)ms') { $avgs += [double]$matches[1] }
        if ($line -match 'p99=([0-9.]+)ms') { $p99s += [double]$matches[1] }
    }
    return [ordered]@{
        tag           = $Tag
        frame_samples = $p99s.Count
        avg_mean      = if ($avgs.Count) { [Math]::Round(($avgs | Measure-Object -Average).Average, 3) } else { $null }
        p99_mean      = if ($p99s.Count) { [Math]::Round(($p99s | Measure-Object -Average).Average, 3) } else { $null }
        p99_max       = if ($p99s.Count) { [Math]::Round(($p99s | Measure-Object -Maximum).Maximum, 3) } else { $null }
    }
}

# Interleaved repeats: ON, OFF, ON, OFF. Interleaving matters because machine load
# drifts over minutes; running all ONs then all OFFs would attribute that drift to
# the feature.
$onRuns = @()
$offRuns = @()
for ($r = 1; $r -le $Repeats; $r++) {
    $onRuns += Invoke-Run $true "preview_on_$r"
    $offRuns += Invoke-Run $false "preview_off_$r"
}

function Get-P99Series($runs) {
    @($runs | Where-Object { $_.p99_mean -ne $null } | ForEach-Object { $_.p99_mean })
}
$onSeries = Get-P99Series $onRuns
$offSeries = Get-P99Series $offRuns

function Get-Spread($series) {
    if ($series.Count -lt 2) { return $null }
    return [Math]::Round((($series | Measure-Object -Maximum).Maximum - ($series | Measure-Object -Minimum).Minimum), 3)
}

# Bare OS timer jitter with no application behind it: the floor every frame budget on
# this machine is measured against.
function Get-BareTimerJitter([int]$Frames = 1200) {
    $samples = [T016Bench]::SampleTimerJitter($Frames)
    if (-not $samples -or $samples.Count -eq 0) { return $null }
    $sorted = @($samples | Sort-Object)
    $at = { param($q) $sorted[[int][Math]::Floor($sorted.Count * $q)] }
    return [ordered]@{
        frames = $sorted.Count
        mean   = [Math]::Round(($sorted | Measure-Object -Average).Average, 3)
        p50    = [Math]::Round((& $at 0.50), 3)
        p99    = [Math]::Round((& $at 0.99), 3)
        max    = [Math]::Round(($sorted | Measure-Object -Maximum).Maximum, 3)
    }
}

$onMean = if ($onSeries.Count) { [Math]::Round(($onSeries | Measure-Object -Average).Average, 3) } else { $null }
$offMean = if ($offSeries.Count) { [Math]::Round(($offSeries | Measure-Object -Average).Average, 3) } else { $null }
$onSpread = Get-Spread $onSeries
$offSpread = Get-Spread $offSeries
# The noise floor is the larger of the two same-config spreads. A delta inside it is
# indistinguishable from repeating the same build twice.
$noise = @($onSpread, $offSpread) | Where-Object { $_ -ne $null } | Measure-Object -Maximum
$noiseFloor = if ($noise.Count) { $noise.Maximum } else { $null }

$delta = if ($onMean -ne $null -and $offMean -ne $null) {
    [Math]::Round($onMean - $offMean, 3)
} else { $null }

# Reference floor: the same high-resolution waitable timer AppLoop uses, with no
# application behind it. This separates "the dock is over budget" from "this machine
# cannot schedule a 16ms timer on budget". Measured on this box: bare timer
# p99=17.31ms, max=17.96ms - already over 16.7ms with no app running. Any dock
# reading above that number carries the scheduler's cost, not the feature's.
$bare = Get-BareTimerJitter 1200

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("dpi=$Dpi seconds=$Seconds repeats=$Repeats")
$lines.Add('--- preview ON ---')
foreach ($r in $onRuns) { $lines.Add("run=$($r.tag) frame_samples=$($r.frame_samples) avg_mean=$($r.avg_mean) p99_mean=$($r.p99_mean) p99_max=$($r.p99_max) preview_log_lines=$($r.preview_log_lines)") }
$lines.Add("p99_mean_of_means=$onMean p99_spread=$onSpread")
$lines.Add('--- preview OFF ---')
foreach ($r in $offRuns) { $lines.Add("run=$($r.tag) frame_samples=$($r.frame_samples) avg_mean=$($r.avg_mean) p99_mean=$($r.p99_mean) p99_max=$($r.p99_max) preview_log_lines=$($r.preview_log_lines)") }
$lines.Add("p99_mean_of_means=$offMean p99_spread=$offSpread")
$lines.Add('--- bare OS timer jitter (same timer, no app) ---')
if ($bare) {
    $lines.Add("frames=$($bare.frames) mean=$($bare.mean) p50=$($bare.p50) p99=$($bare.p99) max=$($bare.max)")
} else {
    $lines.Add('frames=0 (timer sampling failed)')
}
$lines.Add('--- attribution ---')
$lines.Add("p99_delta_ms=$delta noise_floor_ms=$noiseFloor")
if ($delta -eq $null) {
    $lines.Add('attribution=NO_FRAME_SAMPLES (set DOCKFORGE_LOG_LEVEL=debug and wait for frames=1000)')
} elseif ($noiseFloor -ne $null -and [Math]::Abs($delta) -le $noiseFloor) {
    $lines.Add('attribution=NO_SIGNAL (delta within same-config spread; the preview is indistinguishable from no preview)')
} else {
    $lines.Add('attribution=SIGNAL (delta exceeds same-config spread; inspect magnitude)')
}

# Budget is judged against the bare timer floor, not against a flat 16.7ms. The floor
# on this machine is already over 16.7ms with no application running, so a flat
# comparison would blame the dock for the host's scheduling jitter. What matters is
# how far the dock sits above that floor.
$onP99Max = if ($onRuns.Count) { ($onRuns | Where-Object { $_.p99_max -ne $null } | ForEach-Object { $_.p99_max } | Measure-Object -Maximum).Maximum } else { $null }
$onP99Best = if ($onRuns.Count) { ($onRuns | Where-Object { $_.p99_max -ne $null } | ForEach-Object { $_.p99_max } | Measure-Object -Minimum).Minimum } else { $null }
if ($onP99Max -eq $null) {
    $lines.Add('budget=UNKNOWN (no frame samples)')
} elseif (-not $bare) {
    $lines.Add("budget=OVER_UNATTRIBUTED (quietest preview_on p99_max=$onP99Best ms; no timer floor to compare against)")
} elseif ($onP99Best -le 16.7) {
    $lines.Add("budget=WITHIN (quietest preview_on p99_max=$onP99Best ms <= 16.7ms; noisiest=$onP99Max ms is host load)")
} else {
    # Above 16.7ms: is the dock above the machine's own floor?
    $overFloor = [Math]::Round($onP99Best - $bare.p99, 3)
    if ($overFloor -le 0) {
        $lines.Add("budget=FLOOR_LIMITED (preview_on p99_max=$onP99Best ms > 16.7ms, but the bare timer already measures p99=$($bare.p99)ms on this host; the dock is at or below the machine's own floor, so the excess is scheduling jitter, not the feature)")
    } else {
        $lines.Add("budget=OVER_FLOOR (preview_on p99_max=$onP99Best ms is $overFloor ms above the bare timer p99=$($bare.p99)ms; that gap is the dock's own cost)")
    }
}
$lines | ForEach-Object { Write-Output $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
