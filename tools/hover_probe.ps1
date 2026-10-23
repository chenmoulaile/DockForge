# T-010 hover driver: move the pointer across the dock and sample pixels.
#
# Runs the app, waits for the window, then walks the cursor across the dock in
# steps, screenshotting at each phase. The dock must magnify under the pointer and
# return to rest after the pointer leaves.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$ProcessName = 'DockForge',
    [int]$Steps = 6,
    [int]$SettleMs = 260
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class T010 {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static IntPtr Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); return IntPtr.Zero; }
}
"@
[T010]::Enable() | Out-Null

function Get-DockRect {
    param([int]$TargetPid)
    $found = [IntPtr]::Zero
    $cb = [T010+EnumProc]{
        param($h, $p)
        $owner = 0
        [T010]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
        if ($owner -eq $TargetPid) {
            $sb = New-Object System.Text.StringBuilder 256
            [T010]::GetClassName($h, $sb, 256) | Out-Null
            if ($sb.ToString() -eq 'DockForgeHostWindow') {
                $script:foundHandle = $h
                return $false
            }
        }
        return $true
    }
    $script:foundHandle = [IntPtr]::Zero
    [T010]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    $found = $script:foundHandle
    if ($found -eq [IntPtr]::Zero) { throw 'dock window not found' }
    $r = New-Object T010+RECT
    [T010]::GetWindowRect($found, [ref]$r) | Out-Null
    return $r
}

function Capture-Rect {
    param([string]$Path, $Rect)
    $w = $Rect.Right - $Rect.Left
    $h = $Rect.Bottom - $Rect.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($Rect.Left, $Rect.Top, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return "$w x $h"
}

$lines = New-Object System.Collections.Generic.List[string]
$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if ($proc -eq $null) {
    Start-Process -FilePath $Exe | Out-Null
    Start-Sleep -Milliseconds 1500
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
    if ($proc -eq $null) { throw 'process did not start' }
}
$targetPid = $proc.Id
Start-Sleep -Milliseconds 500

$r = Get-DockRect -TargetPid $targetPid
$lines.Add("window rect=$($r.Left),$($r.Top) size=$($r.Right - $r.Left)x$($r.Bottom - $r.Top)")
$lines.Add("pid=$targetPid")

$centreY = [int](($r.Top + $r.Bottom) / 2)
$width = $r.Right - $r.Left

# Park the cursor well away first so the first move into the dock is a real
# transition rather than a no-op at the current position.
[T010]::SetCursorPos(($r.Left - 300), ($r.Top - 300)) | Out-Null
Start-Sleep -Milliseconds 400
$idle = "$Out.idle.png"
$size = Capture-Rect -Path $idle -Rect $r
$lines.Add("idle captured $size -> $idle")

for ($i = 1; $i -le $Steps; $i++) {
    $x = [int]($r.Left + $width * $i / ($Steps + 1))
    [T010]::SetCursorPos($x, $centreY) | Out-Null
    Start-Sleep -Milliseconds $SettleMs
    $shot = "$Out.step$i.png"
    $size = Capture-Rect -Path $shot -Rect $r
    $lines.Add("step$i cursor=$x,$centreY captured $size -> $shot")
}

# Leave and confirm the dock settles back.
[T010]::SetCursorPos(($r.Left - 300), ($r.Top - 300)) | Out-Null
Start-Sleep -Milliseconds 500
$after = "$Out.after.png"
$size = Capture-Rect -Path $after -Rect $r
$lines.Add("after captured $size -> $after")

$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($Out, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
