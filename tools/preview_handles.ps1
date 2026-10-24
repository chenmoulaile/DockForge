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
    [int]$ShortDwellMs = 250
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

function Get-DockRect([int]$Pid) {
    $found = [IntPtr]::Zero
    $cb = [T016Handles+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T016Handles]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $Pid) { return $true }
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
if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }

$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs
$rect = Get-DockRect $proc.Id
if (-not $rect) { throw 'dock window not found' }

$centreX = [int](($rect.Left + $rect.Right) / 2)
$h = $rect.Bottom - $rect.Top
$plateY = [int]($rect.Bottom - $h * 0.25)
$offX = $rect.Left - 500
$offY = $rect.Top - 500

# Baseline after the app has settled: the first frames allocate the D2D device,
# the icon bitmaps and the plate geometry, and those are one-time costs that must
# not be attributed to the show/hide loop.
[void][T016Handles]::SetCursorPos($offX, $offY)
Start-Sleep -Milliseconds 800
$gdiBefore = [T016Handles]::GdiObjects($proc.Id)
$userBefore = [T016Handles]::UserObjects($proc.Id)
$offset = Get-LogLength

# One cycle = hover until the popup shows, then leave so HidePreview runs. Both
# halves matter: showing allocates thumbnails, hiding must release them.
for ($i = 1; $i -le $Cycles; $i++) {
    [void][T016Handles]::SetCursorPos($centreX, $plateY)
    Start-Sleep -Milliseconds $LongDwellMs
    [void][T016Handles]::SetCursorPos($offX, $offY)
    Start-Sleep -Milliseconds $ShortDwellMs + 150
}

Start-Sleep -Milliseconds 800
$gdiAfter = [T016Handles]::GdiObjects($proc.Id)
$userAfter = [T016Handles]::UserObjects($proc.Id)

$tail = Read-LogFrom $offset
$shown = @($tail -split "`n" | Where-Object { $_ -match 'preview shown tile=' }).Count
$hidden = @($tail -split "`n" | Where-Object { $_ -match 'preview hidden' }).Count
$registered = @($tail -split "`n" | Where-Object { $_ -match 'registered' }).Count

$report = New-Object System.Collections.Generic.List[string]
$report.Add("cycles=$Cycles dpi=$Dpi pid=$($proc.Id)")
$report.Add("gdi_objects_before=$gdiBefore gdi_objects_after=$gdiAfter delta=$($gdiAfter - $gdiBefore)")
$report.Add("user_objects_before=$userBefore user_objects_after=$userAfter delta=$($userAfter - $userBefore)")
$report.Add("log_shown_lines=$shown log_hidden_lines=$hidden log_registered_lines=$registered")
$report.Add('--- verdict ---')
$ok = $true
if ($shown -lt $Cycles) { $report.Add("FAIL: only $shown of $Cycles dwells produced a popup"); $ok = $false }
if ($hidden -lt $Cycles) { $report.Add("FAIL: only $hidden of $Cycles leaves produced a hide (preview may be stuck)"); $ok = $false }
if ($gdiAfter -lt 0 -or $userAfter -lt 0) { $report.Add('FAIL: could not read GetGuiResources (OpenProcess refused)'); $ok = $false }
else {
    if ($gdiAfter -ne $gdiBefore) { $report.Add("FAIL: GDI object count changed by $($gdiAfter - $gdiBefore)"); $ok = $false }
    if ($userAfter -ne $userBefore) { $report.Add("FAIL: USER object count changed by $($userAfter - $userBefore)"); $ok = $false }
}
$report.Add("verdict=$(if ($ok) { 'PASS' } else { 'FAIL' })")

$outPath = Join-Path $OutDir 'preview_handles.txt'
[System.IO.File]::WriteAllText($outPath, ($report -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
$report | ForEach-Object { Write-Output $_ }
