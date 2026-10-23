# Sweep the pointer across the dock repeatedly and report the app's own frame stats.
#
# The dock only repaints when hover moves it, so idle numbers say nothing about the
# animation budget. This keeps the pointer in motion for the whole window and then
# reads the p50/p99 lines the app logs every second.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = 'DockForge',
    [int]$Seconds = 20,
    [int]$SweepMs = 12
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T010B {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static void Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
}
"@
[T010B]::Enable()

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if ($proc -eq $null) { throw "process $ProcessName is not running" }
$targetPid = $proc.Id

$script:h = [IntPtr]::Zero
$cb = [T010B+EnumProc]{
    param($w, $p)
    $o = 0
    [T010B]::GetWindowThreadProcessId($w, [ref]$o) | Out-Null
    if ($o -eq $targetPid) {
        $sb = New-Object System.Text.StringBuilder 256
        [T010B]::GetClassName($w, $sb, 256) | Out-Null
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:h = $w; return $false }
    }
    return $true
}
[T010B]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
if ($script:h -eq [IntPtr]::Zero) { throw 'dock window not found' }

$r = New-Object T010B+RECT
[T010B]::GetWindowRect($script:h, [ref]$r) | Out-Null
$w = $r.Right - $r.Left

$log = Get-ChildItem "$env:LOCALAPPDATA\DockForge\logs\*.log" |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log -eq $null) { throw 'no log file' }
$startOffset = (Get-Item $log.FullName).Length

$y = [int]($r.Bottom - 30)
$deadline = (Get-Date).AddSeconds($Seconds)
$sweeps = 0
while ((Get-Date) -lt $deadline) {
    # Ping-pong across the plate so the ramp is exercised in both directions.
    for ($x = $r.Left + 8; $x -lt $r.Right - 8; $x += 24) {
        [T010B]::SetCursorPos($x, $y) | Out-Null
        Start-Sleep -Milliseconds $SweepMs
    }
    for ($x = $r.Right - 8; $x -gt $r.Left + 8; $x -= 24) {
        [T010B]::SetCursorPos($x, $y) | Out-Null
        Start-Sleep -Milliseconds $SweepMs
    }
    $sweeps++
    if ((Get-Date) -ge $deadline) { break }
}

# Park the pointer off the dock so the final sample is not mid-ramp.
[T010B]::SetCursorPos(($r.Left - 400), ($r.Top - 400)) | Out-Null
Start-Sleep -Milliseconds 400

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("sweeps=$sweeps seconds=$Seconds sweep_ms=$SweepMs")
$lines.Add("log=$($log.FullName)")
$lines.Add("--- frame lines since start ---")

# The app holds the log open for appending. ReadAllText asks for FileShare.Read,
# which the app's handle refuses; open explicitly sharing read+write instead.
$stream = [System.IO.File]::Open($log.FullName, [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
$reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
$text = $reader.ReadToEnd()
$reader.Dispose()
$stream.Dispose()
$tail = $text.Substring([Math]::Min($startOffset, $text.Length))
$frameLines = @()
foreach ($line in ($tail -split "`n")) {
    if ($line -match 'frames=\d+') { $frameLines += $line.TrimEnd() }
}
foreach ($line in $frameLines) { $lines.Add($line) }

$p99s = @()
$avgs = @()
foreach ($line in $frameLines) {
    if ($line -match 'avg=([0-9.]+)ms') { $avgs += [double]$matches[1] }
    if ($line -match 'p99=([0-9.]+)ms') { $p99s += [double]$matches[1] }
}

if ($p99s.Count -gt 0) {
    $over = @($p99s | Where-Object { $_ -gt 16.7 })
    $lines.Add("--- summary over $($p99s.Count) one-second windows ---")
    $lines.Add("avg_mean=$([Math]::Round(($avgs | Measure-Object -Average).Average, 3))ms")
    $lines.Add("p99_mean=$([Math]::Round(($p99s | Measure-Object -Average).Average, 3))ms")
    $lines.Add("p99_max=$([Math]::Round(($p99s | Measure-Object -Maximum).Maximum, 3))ms")
    $lines.Add("p99_over_budget_windows=$($over.Count)")
    $lines.Add("verdict=$(if ($over.Count -eq 0) { 'WITHIN_BUDGET' } else { 'OVER_BUDGET' })")
} else {
    $lines.Add('verdict=NO_FRAME_SAMPLES (set DOCKFORGE_LOG_LEVEL=debug)')
}

$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
