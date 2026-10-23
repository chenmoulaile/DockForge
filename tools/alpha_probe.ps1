# tools/alpha_probe.ps1 - prove per-pixel transparency without trusting the eye.
#
# A WS_EX_NOREDIRECTIONBITMAP + DirectComposition window has no redirection
# bitmap, so "the pixel is dark" proves nothing: the wallpaper could be dark.
# The only honest test is differential: sample the same screen region twice, once
# while the app is painted and once with the app gone, and compare.
#
# The second sample is taken after the process is KILLED rather than after
# ShowWindow(SW_HIDE). Hiding is asynchronous through DWM and produced
# contradictory readings during T-004 (the "hidden" grab sometimes still
# contained the window). Killing is deterministic.
#
#   pwsh -File tools/alpha_probe.ps1 -Out evidence/T-004/alpha192.txt -Auto
#   pwsh -File tools/alpha_probe.ps1 -Out x.txt -Points "center:264,76;corner:3,3"
#
# The caller is responsible for having the app running; this script kills it.
param(
    [Parameter(Mandatory = $true)][string]$Out,
    # "name:x,y" pairs relative to the window's top-left, physical px. Omit and
    # pass -Auto to derive a standard set from the live window size.
    [string]$Points = '',
    [switch]$Auto,
    [string]$ProcessName = 'DockForge',
    # Colour distance below which two samples count as identical.
    [int]$Tolerance = 2
)

# A probe that silently reports "all transparent" when it failed to find the
# window is worse than no probe: it manufactures fake evidence. Stop on the
# first error instead.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

[Probe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

# The DockForge window is WS_EX_TOOLWINDOW, so Process.MainWindowHandle is 0.
# Find it by class name + owning process instead. $pid is a read-only PowerShell
# automatic variable, hence $targetPid.
function Find-DockForgeWindow([int]$targetPid) {
    $script:found = [IntPtr]::Zero
    $script:targetPid = $targetPid
    $cb = [Probe+EnumProc] {
        param($h, $p)
        $sb = New-Object System.Text.StringBuilder 256
        [Probe]::GetClassNameW($h, $sb, 256) | Out-Null
        if ($sb.ToString() -ne 'DockForgeHostWindow') { return $true }
        [uint32]$owner = 0
        [Probe]::GetWindowThreadProcessId($h, [ref]$owner) | Out-Null
        if ($owner -eq $script:targetPid) {
            $script:found = $h
            return $false
        }
        return $true
    }
    [Probe]::EnumWindows($cb, [IntPtr]::Zero) | Out-Null
    return $script:found
}

function Grab([int]$x, [int]$y, [int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($x, $y, 0, 0, (New-Object System.Drawing.Size $w, $h))
    $g.Dispose()
    return $bmp
}

$proc = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$hwnd = Find-DockForgeWindow $proc.Id
if ($hwnd -eq [IntPtr]::Zero) { throw "no DockForgeHostWindow found for pid $($proc.Id)" }

$rect = New-Object Probe+RECT
[Probe]::GetWindowRect($hwnd, [ref]$rect) | Out-Null
$x0 = $rect.Left; $y0 = $rect.Top
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "window rect is empty: ${w}x${h}" }

if ($Auto) {
    # Dead centre (painted), four corners just outside the rounded arc
    # (transparent), top edge midpoint (painted - the hairline border).
    $Points = "center:$([int]($w / 2)),$([int]($h / 2));" +
              "corner_tl:3,3;corner_tr:$($w - 4),3;" +
              "corner_bl:3,$($h - 4);corner_br:$($w - 4),$($h - 4);" +
              "top_mid:$([int]($w / 2)),2"
}

$parsed = @()
foreach ($pair in $Points.Split(';')) {
    if (-not $pair.Trim()) { continue }
    $name, $xy = $pair.Split(':')
    $px, $py = $xy.Split(',')
    $parsed += [pscustomobject]@{ Name = $name; X = [int]$px; Y = [int]$py }
}

# Give the compositor a moment so the shown grab is the settled frame.
Start-Sleep -Milliseconds 400
$shown = Grab $x0 $y0 $w $h

Stop-Process -Id $proc.Id -Force
Start-Sleep -Milliseconds 500
$hidden = Grab $x0 $y0 $w $h

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

$lines = @()
$lines += "window=$x0,$y0 size=${w}x${h} pid=$($proc.Id)"
$lines += "rule=unchanged means TRANSPARENT (desktop shows through); changed means PAINTED (tolerance=$Tolerance)"
$lines += ('{0,-12} {1,-12} {2,-16} {3,-16} {4}' -f 'point', 'xy', 'shown', 'hidden', 'verdict')

$painted = 0
$transparent = 0
foreach ($p in $parsed) {
    $c1 = $shown.GetPixel($p.X, $p.Y)
    $c2 = $hidden.GetPixel($p.X, $p.Y)
    $delta = [Math]::Max([Math]::Max([Math]::Abs($c1.R - $c2.R), [Math]::Abs($c1.G - $c2.G)),
                         [Math]::Abs($c1.B - $c2.B))
    if ($delta -le $Tolerance) { $transparent++; $verdict = 'TRANSPARENT' }
    else { $painted++; $verdict = 'PAINTED' }
    $lines += ('{0,-12} {1,-12} {2,-16} {3,-16} {4}' -f $p.Name, "$($p.X),$($p.Y)",
        "$($c1.R),$($c1.G),$($c1.B)", "$($c2.R),$($c2.G),$($c2.B)", $verdict)
}
$lines += "summary: painted=$painted transparent=$transparent"
$shown.Dispose()
$hidden.Dispose()

Set-Content -LiteralPath $Out -Value $lines -Encoding UTF8
$lines
