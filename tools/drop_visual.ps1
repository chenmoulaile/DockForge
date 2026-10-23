# T-014 visual evidence: captures the drop caret while an external drag is over the dock.
#
# The caret only exists during the drag, so the screenshot has to be taken while the
# source process is holding the button down. spikes/t014_drop's --dwell-ms keeps it
# there long enough to capture; without a dwell the release lands before the grab.
#
# The idle frame is captured first, from the same window, so the difference is
# attributable to the drag and not to a stale or moved window.
param(
    [string]$OutDir = 'evidence\T-014',
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$SpikeExe = 'spikes\t014_drop\build\spike_t014_drop.exe',
    [string]$Payload = '',
    [int]$Dpi = 192,
    [int]$DwellMs = 3000,
    [int]$SettleMs = 3000,
    [string]$ForceMaterial = 'solid'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T014Vis {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@ -ErrorAction SilentlyContinue
[void][T014Vis]::SetProcessDpiAwarenessContext([IntPtr](-4))

function Get-DockRect {
    $target = (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1)
    if (-not $target) { return $null }
    $script:found = [IntPtr]::Zero
    $cb = [T014Vis+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T014Vis]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $target.Id) { return $true }
        $sb = New-Object System.Text.StringBuilder 256
        [void][T014Vis]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:found = $h; return $false }
        return $true
    }
    [void][T014Vis]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:found -eq [IntPtr]::Zero) { return $null }
    $r = New-Object T014Vis+RECT
    [void][T014Vis]::GetWindowRect($script:found, [ref]$r)
    return $r
}

function Save-Shot([string]$Path) {
    $r = Get-DockRect
    if (-not $r) { throw 'dock window not found' }
    $w = $r.Right - $r.Left
    $h = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap -ArgumentList @($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size($w, $h)))
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return "rect=$($r.Left),$($r.Top) ${w}x${h}"
}

if (-not $Payload) {
    $Payload = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Cursor.lnk'
}
$tag = if ($Payload -like '*.txt') { 'reject' } else { 'accept' }

Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Get-Process -Name 'spike_t014_drop' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$configPath = Join-Path $env:APPDATA 'DockForge\config.json'
if (Test-Path "$configPath.t014bak") { Copy-Item "$configPath.t014bak" $configPath -Force }

$env:DOCKFORGE_LOG_LEVEL = 'debug'
if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }
# The frost tier puts the window under WDA_EXCLUDEFROMCAPTURE so it cannot sample itself,
# and the side effect is that CopyFromScreen returns the desktop behind it - i.e. a black
# rectangle where the dock is. Solid renders without a capture, so the window is
# capturable. This is a limitation of the shutter, not of the caret.
if ($ForceMaterial) { $env:DOCKFORGE_FORCE_MATERIAL = $ForceMaterial } else { Remove-Item Env:\DOCKFORGE_FORCE_MATERIAL -ErrorAction SilentlyContinue }
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs

# Park the pointer away from the dock so the idle frame has no hover ramp in it.
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public static class T014Cur { [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y); }' -ErrorAction SilentlyContinue
[void][T014Cur]::SetCursorPos(20, 20)
Start-Sleep -Milliseconds 600

$idlePath = Join-Path $OutDir "drop_idle_$tag`_dpi$Dpi.png"
$idleInfo = Save-Shot $idlePath

$r = Get-DockRect
$centreX = [int](($r.Left + $r.Right) / 2)
$plateY = [int]($r.Bottom - ($r.Bottom - $r.Top) * 0.25)

$spikeOut = Join-Path $OutDir "spike_visual_$tag`_dpi$Dpi.txt"
if (Test-Path $spikeOut) { Remove-Item $spikeOut -Force }
$args = @("--mode=drag", "--payload=`"$Payload`"", "--to=$centreX,$plateY",
          "--hold-ms=9000", "--dwell-ms=$DwellMs", "--out=$spikeOut")
# Capture timing is driven by the app's own "drop enter" line, not by a fixed sleep.
# A fixed sleep was measured to fire ~200ms BEFORE the drag entered (the driver spends
# ~4s arming and travelling), which produced an IDENTICAL diff and would have been read
# as "the caret is not drawn" rather than "the shutter was early".
$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
$log = Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$logStart = $log.Length
$spike = Start-Process -FilePath $SpikeExe -ArgumentList $args -PassThru

$entered = $false
for ($i = 0; $i -lt 200; ++$i) {
    Start-Sleep -Milliseconds 50
    $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        if ($logStart -gt $fs.Length) { $logStart = 0 }
        $fs.Position = $logStart
        $sr = New-Object System.IO.StreamReader($fs)
        $tail = $sr.ReadToEnd()
    } finally { $fs.Close() }
    if ($tail -match 'drop enter') { $entered = $true; break }
    if ($spike.HasExited) { break }
}
if (-not $entered) { throw 'drag never entered the dock; the caret cannot be captured' }
# Halfway through the dwell, so neither the enter nor the release is on the shutter.
Start-Sleep -Milliseconds ([Math]::Max(200, [int]($DwellMs / 2)))
$dragPath = Join-Path $OutDir "drop_caret_$tag`_dpi$Dpi.png"
$dragInfo = Save-Shot $dragPath
[void]$spike.WaitForExit(20000)
if (-not $spike.HasExited) { $spike | Stop-Process -Force }

& "$PSScriptRoot\image_diff.ps1" -A $idlePath -B $dragPath -Out (Join-Path $OutDir "drop_diff_$tag`_dpi$Dpi.txt") | Out-Null
$diff = [System.IO.File]::ReadAllText((Join-Path $OutDir "drop_diff_$tag`_dpi$Dpi.txt"), [System.Text.Encoding]::UTF8)

$summary = @()
$summary += "dpi=$Dpi payload=$Payload tag=$tag"
$summary += "idle: $idleInfo -> $idlePath"
$summary += "drag: $dragInfo -> $dragPath"
$summary += '--- image_diff ---'
$summary += ($diff -split "`n" | Where-Object { $_ -match 'changed|bbox|verdict|rows_with|cols_with' })
$outPath = Join-Path $OutDir "drop_visual_$tag`_dpi$Dpi.txt"
[System.IO.File]::WriteAllText($outPath, ($summary -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
$summary -join "`n"
