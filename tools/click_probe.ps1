# Clicks one Dock slot and reports what the app did with it.
#
# The click is injected with SendInput, which is a real hardware-level event: it goes
# through the same path a mouse does, so foreground rules apply exactly as they would
# for a user. That is the point - a SetForegroundWindow call from this script would be
# refused, but the click it causes grants the right to the window under the cursor.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$IconIndex = 0,
    [int]$TileCount = 12,
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [int]$SettleMs = 1200,
    [switch]$SecondClick
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class ClickNative {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, IntPtr e);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@

[void][ClickNative]::SetProcessDpiAwarenessContext([IntPtr]::new(-4))

function Get-DockWindow {
    param([int]$TargetPid)
    $found = [IntPtr]::Zero
    $cb = [ClickNative+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][ClickNative]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $TargetPid) { return $true }
        $sb = [System.Text.StringBuilder]::new(256)
        [void][ClickNative]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') {
            $script:dockHwnd = $h
            return $false
        }
        return $true
    }
    $script:dockHwnd = [IntPtr]::Zero
    [void][ClickNative]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:dockHwnd
}

function Get-ForegroundInfo {
    $h = [ClickNative]::GetForegroundWindow()
    $procId = 0
    [void][ClickNative]::GetWindowThreadProcessId($h, [ref]$procId)
    $sb = [System.Text.StringBuilder]::new(512)
    [void][ClickNative]::GetWindowText($h, $sb, 512)
    $cls = [System.Text.StringBuilder]::new(256)
    [void][ClickNative]::GetClassName($h, $cls, 256)
    $proc = Get-Process -Id $procId -ErrorAction SilentlyContinue
    return [pscustomobject]@{
        hwnd  = $h
        pid   = $procId
        name  = if ($proc) { $proc.ProcessName } else { '?' }
        cls   = $cls.ToString()
        title = $sb.ToString()
    }
}

$lines = [System.Collections.Generic.List[string]]::new()
function Add-Line { param([string]$Text) $lines.Add($Text); Write-Host $Text }

Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs
if ($proc.HasExited) { throw "app exited during startup with code $($proc.ExitCode)" }

$dock = Get-DockWindow -TargetPid $proc.Id
if ($dock -eq [IntPtr]::Zero) { throw 'DockForgeHostWindow not found' }

$rect = New-Object ClickNative+RECT
[void][ClickNative]::GetWindowRect($dock, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Add-Line "dock=($($rect.Left),$($rect.Top)) ${width}x${height} pid=$($proc.Id)"

# Geometry mirrors dock::ComputeGeometry. Scale comes from the WINDOW HEIGHT, not the
# width: the window is sized to the widest row the pointer can produce, so width is
# always larger than plate + padding and dividing by it yields a scale that is too
# high. Height is exact - icon*max(mag,1) + 2*padding - because it never depends on
# hover (measured: 213px at 192 DPI for 48 DIP icons at 1.8x).
$contentDip = $TileCount * 48 + ($TileCount - 1) * 8
$windowHeightDip = 48 * 1.8 + 20.0
$scale = $height / $windowHeightDip
$windowWidthDip = $width / $scale
# The row is centred in the window, not offset by the padding.
$originDip = ($windowWidthDip - $contentDip) / 2.0
$xDip = $originDip + $IconIndex * 56 + 24
$clickX = [int][Math]::Round($rect.Left + $xDip * $scale)
$clickY = [int][Math]::Round($rect.Bottom - 30 * $scale)
Add-Line ("target tile=$IconIndex scale={0:F3} window={1:F1}dip origin={2:F1}dip click=({3},{4})" -f $scale, $windowWidthDip, $originDip, $clickX, $clickY)

# Move first so the hover ramp runs and the region expands to include the icon row.
[void][ClickNative]::SetCursorPos($clickX, $clickY)
Start-Sleep -Milliseconds 500

$hit = [ClickNative]::WindowFromPoint([ClickNative+POINT]@{ X = $clickX; Y = $clickY })
Add-Line "window_from_point=$($hit -eq $dock) ($hit)"

$before = Get-ForegroundInfo
Add-Line "foreground_before=$($before.name) pid=$($before.pid) cls=$($before.cls) title=$($before.title)"

# Down and up as separate calls with a gap: a zero-length press is coalesced by some
# input stacks and the window sees nothing.
[ClickNative]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 60
[ClickNative]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
Start-Sleep -Milliseconds 900

$after = Get-ForegroundInfo
Add-Line "foreground_after=$($after.name) pid=$($after.pid) cls=$($after.cls) title=$($after.title)"
Add-Line "foreground_changed=$($before.hwnd -ne $after.hwnd)"

# Second click on the same slot. This is the branch that distinguishes "raise" from
# "minimize": the first click brought the window forward, so the second must send it
# down. Without this, a dock that only ever launches looks correct until it is used
# twice.
if ($SecondClick) {
    [void][ClickNative]::SetCursorPos($clickX, $clickY)
    Start-Sleep -Milliseconds 300
    [ClickNative]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 60
    [ClickNative]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 900

    $third = Get-ForegroundInfo
    Add-Line "foreground_second=$($third.name) pid=$($third.pid) cls=$($third.cls) title=$($third.title)"
    Add-Line "foreground_second_changed=$($after.hwnd -ne $third.hwnd)"
    # IsIconic on the window that was in front tells us whether it went down rather
    # than merely losing focus to something else.
    $iconic = if ($after.hwnd -ne [IntPtr]::Zero) { [ClickNative]::IsIconic($after.hwnd) } else { $false }
    Add-Line "previous_foreground_iconic=$iconic"
}

Start-Sleep -Milliseconds 400
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue

$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { [void](New-Item -ItemType Directory -Path $dir -Force) }
[System.IO.File]::WriteAllText($Out, ($lines -join "`n") + "`n", (New-Object System.Text.UTF8Encoding($false)))
