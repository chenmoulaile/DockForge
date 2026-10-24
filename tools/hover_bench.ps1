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
    [int]$MoveEveryMs = 0
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T016Bench {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@ -ErrorAction SilentlyContinue
[void][T016Bench]::SetProcessDpiAwarenessContext([IntPtr](-4))

$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'

function Get-LatestLog {
    Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Get-DockRect([int]$Pid) {
    $found = [IntPtr]::Zero
    $cb = [T016Bench+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T016Bench]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $Pid) { return $true }
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

    $proc = Start-Process -FilePath $Exe -PassThru
    Start-Sleep -Milliseconds $SettleMs
    $rect = Get-DockRect $proc.Id
    if (-not $rect) { throw 'dock window not found' }

    $centreX = [int](($rect.Left + $rect.Right) / 2)
    $h = $rect.Bottom - $rect.Top
    $plateY = [int]($rect.Bottom - $h * 0.25)

    $startOffset = (Get-LatestLog).Length
    [void][T016Bench]::SetCursorPos($centreX, $plateY)

    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        # Optional nudge so the OS never coalesces the pointer into a single
        # hover state; default 0 means "never move", which is what the dwell
        # preview needs.
        if ($MoveEveryMs -gt 0) {
            [void][T016Bench]::SetCursorPos(($centreX + 1), $plateY)
            Start-Sleep -Milliseconds $MoveEveryMs
            [void][T016Bench]::SetCursorPos($centreX, $plateY)
        }
        Start-Sleep -Milliseconds 250
    }

    # Park off the dock so the tail sample is not mid-popup.
    [void][T016Bench]::SetCursorPos(($rect.Left - 500), ($rect.Top - 500))
    Start-Sleep -Milliseconds 400

    $tail = Read-LogFrom $startOffset
    $frameLines = @()
    $previewLines = @()
    foreach ($line in ($tail -split "`n")) {
        if ($line -match 'frames=\d+') { $frameLines += $line.TrimEnd() }
        if ($line -match '\[preview\]') { $previewLines += $line.TrimEnd() }
    }

    $p99s = @(); $avgs = @()
    foreach ($line in $frameLines) {
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

$on  = Invoke-Run $true  'preview_on'
$off = Invoke-Run $false 'preview_off'

$delta = if ($on.p99_mean -ne $null -and $off.p99_mean -ne $null) {
    [Math]::Round($on.p99_mean - $off.p99_mean, 3)
} else { $null }

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("dpi=$Dpi seconds=$Seconds")
$lines.Add("--- preview ON ---")
foreach ($k in $on.Keys) { $lines.Add("$k=$($on[$k])") }
$lines.Add("--- preview OFF ---")
foreach ($k in $off.Keys) { $lines.Add("$k=$($off[$k])") }
$lines.Add("--- attribution ---")
$lines.Add("p99_delta_ms=$delta")
if ($delta -eq $null) {
    $lines.Add('verdict=NO_FRAME_SAMPLES (set DOCKFORGE_LOG_LEVEL=debug and wait for frames=1000)')
} elseif ($on.over_budget -gt 0) {
    $lines.Add("verdict=OVER_BUDGET (preview_on has $($on.over_budget) windows over 16.7ms)")
} elseif ([Math]::Abs($delta) -lt 0.018) {
    # 0.018ms is the same-config run-to-run spread measured in T-012; anything
    # under it is indistinguishable from noise and must not be claimed.
    $lines.Add('verdict=WITHIN_BUDGET_AND_NO_SIGNAL (delta below run-to-run noise floor)')
} else {
    $lines.Add('verdict=WITHIN_BUDGET_WITH_SIGNAL (attributable cost, inspect magnitude)')
}

$lines | ForEach-Object { Write-Output $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
