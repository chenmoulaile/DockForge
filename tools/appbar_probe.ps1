# DockForge T-302b evidence probe: the top-strip reservation.
#
# Why this exists. The user asked for a Finder Bar that is permanently pinned. A topmost
# bar over the work area covers the title bar of every maximized window, which is not
# acceptable, so the bar asks the shell to reserve the strip the way the taskbar does.
#
# spikes/T-302_tray/appbar_spike.cpp proved the mechanism (Q10..Q13): ABM_NEW + ABM_SETPOS
# moved the work area from 0,0,2880,1824 to 0,56,2880,1824, a maximized window's top went
# from -13 to 43 (exactly the reserved 56px), and ABM_REMOVE restored it byte for byte.
# SPI_SETWORKAREA - the path Q8 tried and rejected - reports success and changes nothing.
#
# This probe asserts the same three things against the shipping binary, because a spike
# proves the API and only the app can prove the app calls it correctly:
#   A  with the bar on, the work area's top is the reserved height and the snapshot agrees.
#   B  a maximized window really stops below the bar instead of sliding under it.
#   C  turning the bar off gives the strip back - a reservation left behind with no bar on
#      it shortens every maximized window by 28dip and nothing on screen explains why.
#   D  with auto_hide forced on, no strip is reserved at all: the bar retracts instead, and
#      a retracted bar must not reserve space it is not using.
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\appbar_probe.ps1
param(
    [string]$OutDir = 'evidence\T-302',
    [string]$Exe = 'build\bin\DockForge.exe'
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$script:failures = New-Object 'System.Collections.Generic.List[string]'
function Fail([string]$why) {
    Write-Host "FAIL: $why"
    $script:failures.Add($why)
}
function Check([bool]$ok, [string]$what) {
    if ($ok) { Write-Host "  PASS $what" } else { Fail $what }
}
function Stop-All {
    foreach ($n in @('DockForge', 'menu_target')) {
        Get-Process -Name $n -ErrorAction SilentlyContinue | Stop-Process -Force
    }
}

Stop-All
Start-Sleep -Milliseconds 400
if (-not (Test-Path $Exe)) { Write-Host "FAIL: app missing: $Exe"; exit 1 }

$scratch = Join-Path $env:TEMP ('dockforge-t302bar-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch 'config.json'
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
[System.IO.File]::WriteAllText($configPath, '{}', (New-Object System.Text.UTF8Encoding($false)))

$cmdFile = Join-Path $scratch 'bar-cmd.txt'
$stateFile = Join-Path $scratch 'bar-state.json'
New-Item -ItemType File -Force -Path $cmdFile | Out-Null

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_DUMP_DIR = $scratch
$env:DOCKFORGE_FINDERBAR = '1'
$env:DOCKFORGE_FINDERBAR_CMD_FILE = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T302AppBarProbe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    [DllImport("user32.dll")] public static extern bool SystemParametersInfoW(uint action, uint param, out RECT rect, uint winIni);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);

    public static RECT WorkArea() {
        RECT r;
        SystemParametersInfoW(0x0030, 0, out r, 0);  // SPI_GETWORKAREA
        return r;
    }
    public static IntPtr FindExact(string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
[T302AppBarProbe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
function Send-Cmd([string]$text, [int]$settleMs = 400) {
    if (Test-Path $stateFile) { Remove-Item $stateFile -Force }
    $body = "$text`n#" + [guid]::NewGuid().ToString('N')
    [System.IO.File]::WriteAllText($cmdFile, $body, (New-Object System.Text.UTF8Encoding($false)))
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path $stateFile) { break }
    }
    Start-Sleep -Milliseconds $settleMs
}
function Get-State {
    if (-not (Test-Path $stateFile)) { return $null }
    try { Get-Content $stateFile -Raw -Encoding UTF8 | ConvertFrom-Json } catch { $null }
}

# The work area the probe itself sees, before the app touches anything. This is the
# baseline the reservation is measured against, so a machine whose taskbar is already at
# the top still gets a meaningful delta rather than an absolute expectation.
$baseline = [T302AppBarProbe]::WorkArea()
Write-Host ("=== T-302b appbar probe: scratch=$scratch baseline work area = {0},{1},{2},{3} ===" -f `
        $baseline.Left, $baseline.Top, $baseline.Right, $baseline.Bottom)

Start-Process -FilePath (Join-Path $root $Exe) | Out-Null
Start-Sleep -Seconds 4

$app = Get-Process -Name DockForge -ErrorAction SilentlyContinue | Select-Object -First 1
Check ($null -ne $app) 'DockForge is running'

# ---- A: the strip is reserved --------------------------------------------
Write-Host '--- A: reservation ---'
Send-Cmd 'state'
$s = Get-State
Check ($null -ne $s) 'the bar state file was written'
if ($null -ne $s) {
    $reserved = [int]$s.reserved_strip_px
    Write-Host ("  snapshot: auto_hide={0} auto_hiding={1} reserved={2}px height_dip={3} dpi={4}" -f `
            $s.auto_hide, $s.auto_hiding, $reserved, $s.height_dip, $s.dpi)
    Check ($reserved -gt 0) 'the bar reports a reserved strip'
    # 28 dip at 192 dpi is 56px. Derived rather than hard-coded so a different DPI still
    # passes on its own terms.
    $expected = [int]([math]::Round([double]$s.height_dip * [double]$s.dpi / 96.0))
    Write-Host ("  expected strip = {0}px (28 dip at {1} dpi)" -f $expected, $s.dpi)
    Check ($reserved -eq $expected) "the reserved strip matches the bar height ($reserved vs $expected)"
    Check ([bool]$s.auto_hide -eq $false) 'auto_hide is off, so the reservation is the mechanism in use'

    $after = [T302AppBarProbe]::WorkArea()
    Write-Host ("  work area after = {0},{1},{2},{3}" -f $after.Left, $after.Top, $after.Right, $after.Bottom)
    Check ($after.Top -eq ($baseline.Top + $reserved)) `
        "the work area top moved down by exactly the reserved strip ($($after.Top) vs $($baseline.Top + $reserved))"
    Check ($after.Bottom -eq $baseline.Bottom) 'the work area bottom did not move'
    Check ($after.Right -eq $baseline.Right) 'the work area right edge did not move'
}

# ---- B: a maximized window respects it -----------------------------------
Write-Host '--- B: maximize ---'
if ($null -ne $s) {
    $reserved = [int]$s.reserved_strip_px
    # A scratch window of our own, maximized the ordinary way. Using an existing app's
    # window would mean fighting it for foreground and for its own window state.
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class T302MaxProbe {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr CreateWindowExW(
        uint exStyle, string cls, string title, uint style, int x, int y, int w, int h,
        IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool DestroyWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@
    $probe = [T302MaxProbe]::CreateWindowExW(0x00000100, 'static', 'maximize probe', 0x00CF0000,
        200, 300, 640, 420, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
    Check ($probe -ne [IntPtr]::Zero) 'the probe window was created'
    if ($probe -ne [IntPtr]::Zero) {
        [T302MaxProbe]::ShowWindow($probe, 3) | Out-Null   # SW_MAXIMIZE
        [T302MaxProbe]::UpdateWindow($probe) | Out-Null
        Start-Sleep -Milliseconds 500
        $rect = New-Object T302MaxProbe+RECT
        [T302MaxProbe]::GetWindowRect($probe, [ref]$rect) | Out-Null
        Write-Host ("  maximized rect = {0},{1},{2},{3}" -f $rect.Left, $rect.Top, $rect.Right, $rect.Bottom)
        # A maximized window overhangs the work area by its shadow border (13px on this
        # host), so the assertion is a shift, not an absolute top.
        $overhang = 16
        Check ($rect.Top -ge ($baseline.Top + $reserved - $overhang)) `
            "the maximized window stops below the reserved strip (top=$($rect.Top), strip ends at $($baseline.Top + $reserved))"
        [T302MaxProbe]::DestroyWindow($probe) | Out-Null
    }
}

# ---- C: turning the bar off gives the strip back -------------------------
Write-Host '--- C: release ---'
Send-Cmd 'hide'
$s = Get-State
Check ($null -ne $s -and [bool]$s.enabled -eq $false) 'the bar is off'
if ($null -ne $s) {
    Check ([int]$s.reserved_strip_px -eq 0) 'the bar released its reservation when it was turned off'
}
$after = [T302AppBarProbe]::WorkArea()
Write-Host ("  work area after release = {0},{1},{2},{3}" -f $after.Left, $after.Top, $after.Right, $after.Bottom)
Check ($after.Top -eq $baseline.Top) 'the work area top is back to where it started'
Check ($after.Bottom -eq $baseline.Bottom) 'the work area bottom is unchanged after release'

# ---- D: auto_hide reserves nothing --------------------------------------
Write-Host '--- D: auto_hide reserves nothing ---'
# Written as a real config file, then published through the app's own reload path, so the
# reload code is what turns the reservation off rather than the probe reaching inside.
$autoHideConfig = @'
{ "finderbar": { "enabled": true, "auto_hide": true } }
'@
[System.IO.File]::WriteAllText($configPath, $autoHideConfig, (New-Object System.Text.UTF8Encoding($false)))
Send-Cmd 'show'
Start-Sleep -Milliseconds 900
Send-Cmd 'state'
$s = Get-State
if ($null -ne $s) {
    Write-Host ("  auto_hide={0} auto_hiding={1} reserved={2}px progress={3}" -f `
            $s.auto_hide, $s.auto_hiding, $s.reserved_strip_px, $s.progress)
    Check ([bool]$s.auto_hide -eq $true) 'the config change reached the bar'
    Check ([bool]$s.auto_hiding -eq $true) 'the bar reports itself as retracting'
    Check ([int]$s.reserved_strip_px -eq 0) 'an auto-hiding bar reserves no strip'
}
$after = [T302AppBarProbe]::WorkArea()
Check ($after.Top -eq $baseline.Top) 'the work area is untouched while the bar auto-hides'

# ---- verdict ------------------------------------------------------------
Write-Host ''
if ($script:failures.Count -eq 0) {
    Write-Host 'verdict: Q34_strip_reserved=PASS Q35_maximize_respects=PASS Q36_release_restores=PASS Q37_autohide_reserves_none=PASS'
    Stop-All
    exit 0
}
Write-Host ("verdict: FAILED ({0} checks)" -f $script:failures.Count)
foreach ($f in $script:failures) { Write-Host "  - $f" }
Stop-All
exit 1
