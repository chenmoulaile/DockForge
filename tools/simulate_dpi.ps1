# tools/simulate_dpi.ps1 - prove the WM_DPICHANGED handler actually re-places the dock.
#
# A single-monitor host cannot produce a genuine per-monitor DPI transition, and
# the OS refuses to deliver a synthetic one from outside the process:
#   - SendMessage(0x02E0) from another process returns 0 but the handler never runs
#   - PostMessage(0x02E0) fails outright with ERROR_MESSAGE_SYNC_ONLY (1159)
# WM_CLOSE crosses fine, so this is specific to system messages that only have
# meaning when delivered synchronously by the compositor.
#
# The honest test therefore lives in the app: DOCKFORGE_SIMULATE_DPI=<dpi> calls
# SendMessageW on its own HWND once the loop is running, which is the same code
# path the OS uses. This script runs the app twice - once plain, once with the
# override - and compares the window rect, which proves the handler resized and
# repositioned rather than merely logging.
#
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/simulate_dpi.ps1 `
#       -Out evidence/T-007/simulate_dpi.txt -Dpi 96
#
# Honest limitation: GetDpiForWindow keeps reporting the host's real DPI because
# the compositor did not actually move the window between monitors. This proves
# the handler, the placement math and the content/geometry agreement, NOT the
# compositor's own scaling of a real cross-monitor drag.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Dpi = 96,
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# CharSet=CharSet.Unicode makes .NET append "W", so declaring "FindWindowW" would
# look for "FindWindowWW" and always return zero. Use undecorated names.
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class DpiProbe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
}
'@

[DpiProbe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Find-DockWindow {
    param([int]$TargetPid)
    $script:found = [IntPtr]::Zero
    $callback = [DpiProbe+EnumProc] {
        param($h, $p)
        $sb = New-Object System.Text.StringBuilder 256
        [DpiProbe]::GetClassNameW($h, $sb, 256) | Out-Null
        if ($sb.ToString() -eq 'DockForgeHostWindow') {
            $owner = [uint32]0
            [DpiProbe]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
            if ($owner -eq $TargetPid -and [DpiProbe]::IsWindowVisible($h)) {
                $script:found = $h
                return $false
            }
        }
        return $true
    }
    [DpiProbe]::EnumWindows($callback, [IntPtr]::Zero) | Out-Null
    return $script:found
}

function Measure-Dock {
    param([int]$TargetPid, [hashtable]$Env)
    foreach ($key in $Env.Keys) { Set-Item -Path "env:$key" -Value $Env[$key] }
    Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    Start-Process -FilePath $Exe | Out-Null
    $hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 60 -and $hwnd -eq [IntPtr]::Zero; $i++) {
        Start-Sleep -Milliseconds 100
        $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($proc) { $hwnd = Find-DockWindow -TargetPid $proc.Id }
    }
    if ($hwnd -eq [IntPtr]::Zero) { throw "dock window never appeared (exe=$Exe)" }
    # The synthetic message is sent right after the first placement, so wait past
    # it before reading the rect.
    Start-Sleep -Milliseconds 800
    $r = New-Object DpiProbe+RECT
    [DpiProbe]::GetWindowRect($hwnd, [ref]$r) | Out-Null
    $result = [pscustomobject]@{
        Rect = $r
        Width = $r.right - $r.left
        Height = $r.bottom - $r.top
        WindowDpi = [DpiProbe]::GetDpiForWindow($hwnd)
    }
    Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
    return $result
}

$baseline = Measure-Dock -TargetPid 0 -Env @{
    DOCKFORGE_FORCE_MATERIAL = 'solid'
    DOCKFORGE_DPI_OVERRIDE = ''
    DOCKFORGE_SIMULATE_DPI = ''
}
$forced = Measure-Dock -TargetPid 0 -Env @{
    DOCKFORGE_FORCE_MATERIAL = 'solid'
    DOCKFORGE_DPI_OVERRIDE = ''
    DOCKFORGE_SIMULATE_DPI = "$Dpi"
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("requested_dpi=$Dpi")
$lines.Add("rule=the handler must recompute size and position from the work area; a no-op handler leaves the rect unchanged")
$lines.Add(("baseline      {0},{1} {2}x{3} window_dpi={4}" -f $baseline.Rect.left,
        $baseline.Rect.top, $baseline.Width, $baseline.Height, $baseline.WindowDpi))
$lines.Add(("simulated {0,3} {1},{2} {3}x{4} window_dpi={5}" -f $Dpi, $forced.Rect.left,
        $forced.Rect.top, $forced.Width, $forced.Height, $forced.WindowDpi))

$resized = $baseline.Width -ne $forced.Width -or $baseline.Height -ne $forced.Height
$moved = $baseline.Rect.left -ne $forced.Rect.left -or $baseline.Rect.top -ne $forced.Rect.top
$lines.Add("resized=$([int]$resized) moved=$([int]$moved)")
if ($resized -and $moved) {
    $lines.Add("verdict=HANDLER_ACTIVE")
} elseif ($resized) {
    $lines.Add("verdict=HANDLER_RESIZED_ONLY")
} else {
    $lines.Add("verdict=HANDLER_NOOP")
}
$lines.Add("note=window_dpi stays at the host DPI because no real cross-monitor transition occurred")

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
[System.IO.File]::WriteAllText((Join-Path (Resolve-Path -LiteralPath '.').Path $Out), ($lines -join "`r`n") + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
$lines | ForEach-Object { Write-Output $_ }
