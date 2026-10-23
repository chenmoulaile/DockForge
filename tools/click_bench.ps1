# Measures Dock click handling latency by clicking one tile repeatedly.
#
# Reads the app's own per-click timing line rather than measuring from the script:
# the script would include process wake-up, screen capture and PowerShell scheduling,
# which are not what the frame budget cares about. The app logs the span from the
# WM_LBUTTONDOWN handler entry to the action completing.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$IconIndex = 0,
    [int]$TileCount = 12,
    [int]$Clicks = 20,
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$SettleMs = 1400
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class BenchNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

[void][BenchNative]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))

function Get-DockWindow {
    param([int]$TargetPid)
    $script:dockHwnd = [IntPtr]::Zero
    $cb = [BenchNative+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][BenchNative]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $TargetPid) { return $true }
        $sb = [System.Text.StringBuilder]::new(256)
        [void][BenchNative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:dockHwnd = $h; return $false }
        return $true
    }
    [void][BenchNative]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:dockHwnd
}

$lines = [System.Collections.Generic.List[string]]::new()
function Add-Line { param([string]$Text) $lines.Add($Text); Write-Host $Text }

Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
$logBefore = if (Test-Path $logDir) {
    (Get-ChildItem $logDir -Filter *.log | Sort-Object LastWriteTime | Select-Object -Last 1).FullName
} else { '' }
$offsetBefore = if ($logBefore) { (Get-Item $logBefore).Length } else { 0 }

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs
if ($proc.HasExited) { throw "app exited during startup with code $($proc.ExitCode)" }

$dock = Get-DockWindow -TargetPid $proc.Id
if ($dock -eq [IntPtr]::Zero) { throw 'DockForgeHostWindow not found' }

$rect = New-Object BenchNative+RECT
[void][BenchNative]::GetWindowRect($dock, [ref]$rect)
$height = $rect.Bottom - $rect.Top
$width = $rect.Right - $rect.Left
$scale = $height / (48 * 1.8 + 20.0)
$contentDip = $TileCount * 48 + ($TileCount - 1) * 8
$originDip = ($width / $scale - $contentDip) / 2.0
$xDip = $originDip + $IconIndex * 56 + 24
$clickX = [int][Math]::Round($rect.Left + $xDip * $scale)
$clickY = [int][Math]::Round($rect.Bottom - 30 * $scale)
Add-Line ("dock=${width}x${height} scale={0:F3} tile=$IconIndex click=({1},{2})" -f $scale, $clickX, $clickY)

[void][BenchNative]::SetCursorPos($clickX, $clickY)
Start-Sleep -Milliseconds 500

for ($i = 0; $i -lt $Clicks; $i++) {
    [BenchNative]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 40
    [BenchNative]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    # Long enough for a launch to settle so the next click hits the "already running"
    # branch rather than queueing a second launch on top of a starting process.
    Start-Sleep -Milliseconds 700
}

Start-Sleep -Milliseconds 600
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue

$logFile = Get-ChildItem $logDir -Filter *.log | Sort-Object LastWriteTime | Select-Object -Last 1
$fs = [System.IO.File]::Open($logFile.FullName, 'Open', 'Read', 'ReadWrite')
$sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::UTF8)
$text = $sr.ReadToEnd()
$sr.Close(); $fs.Close()
$fresh = $text.Substring([Math]::Min($offsetBefore, $text.Length))

$times = [System.Collections.Generic.List[double]]::new()
$actions = @{}
foreach ($row in ($fresh -split "`n")) {
    if ($row -notmatch '\[dock\] tile\[\d+\] .* -> (\w+) \(([\w-]+), pid=(\d+), ([\d.]+)ms\)') { continue }
    $times.Add([double]$matches[4])
    $key = $matches[1]
    if ($actions.ContainsKey($key)) { $actions[$key] = $actions[$key] + 1 } else { $actions[$key] = 1 }
}

Add-Line "clicks=$($times.Count) requested=$Clicks"
foreach ($k in ($actions.Keys | Sort-Object)) { Add-Line "action_$k=$($actions[$k])" }
if ($times.Count -gt 0) {
    $sorted = $times | Sort-Object
    $sum = ($sorted | Measure-Object -Sum).Sum
    $p = { param($q) $sorted[[Math]::Min($sorted.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($q / 100.0 * $sorted.Count) - 1))] }
    Add-Line ("latency_ms avg={0:F2} p50={1:F2} p95={2:F2} p99={3:F2} max={4:F2}" -f ($sum / $sorted.Count), (& $p 50), (& $p 95), (& $p 99), $sorted[-1])
    $budget = 16.7
    $over = @($sorted | Where-Object { $_ -gt $budget }).Count
    Add-Line "budget_ms=$budget over_budget=$over"
    Add-Line ("verdict={0}" -f $(if ($over -eq 0) { 'WITHIN_BUDGET' } else { 'OVER_BUDGET' }))
}

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
[System.IO.File]::WriteAllText($Out, ($lines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
