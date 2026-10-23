# T-014 evidence: drives a real OLE drag from a separate process onto the live dock.
#
# Why a separate process: DoDragDrop runs a modal loop inside the source, so a
# same-process driver would be testing our own stub. spikes/t014_drop is the source.
#
# Every case records the app's own log lines rather than inferring success from the
# absence of an error: "nothing happened" and "it worked but I did not look" are
# indistinguishable without the log.
param(
    [string]$OutDir = 'evidence\T-014',
    [string]$ProcessName = 'DockForge',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$SpikeExe = 'spikes\t014_drop\build\spike_t014_drop.exe',
    [int]$Dpi = 192,
    [int]$SettleMs = 3000,
    [int]$DwellMs = 300
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$logDir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
$configPath = Join-Path $env:APPDATA 'DockForge\config.json'

function Get-LatestLog {
    Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}

function Read-LogFrom([long]$offset) {
    $log = Get-LatestLog
    $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        if ($offset -gt $fs.Length) { $offset = 0 }
        $fs.Position = $offset
        $sr = New-Object System.IO.StreamReader($fs)
        return $sr.ReadToEnd()
    } finally {
        $fs.Close()
    }
}

function Get-LogLength {
    $log = Get-LatestLog
    return $log.Length
}

function Get-DockRect {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class T014Win {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@ -ErrorAction SilentlyContinue
    [void][T014Win]::SetProcessDpiAwarenessContext([IntPtr](-4))
    $target = (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1)
    if (-not $target) { return $null }
    $found = [IntPtr]::Zero
    $cb = [T014Win+EnumProc] {
        param($h, $p)
        $procId = 0
        [void][T014Win]::GetWindowThreadProcessId($h, [ref]$procId)
        if ($procId -ne $target.Id) { return $true }
        $sb = New-Object System.Text.StringBuilder 256
        [void][T014Win]::GetClassName($h, $sb, 256)
        if ($sb.ToString() -eq 'DockForgeHostWindow') { $script:dockHwnd = $h; return $false }
        return $true
    }
    $script:dockHwnd = [IntPtr]::Zero
    [void][T014Win]::EnumWindows($cb, [IntPtr]::Zero)
    if ($script:dockHwnd -eq [IntPtr]::Zero) { return $null }
    $r = New-Object T014Win+RECT
    [void][T014Win]::GetWindowRect($script:dockHwnd, [ref]$r)
    return $r
}

function Invoke-Drag([string]$Payload, [int]$TargetX, [int]$TargetY, [string]$Tag) {
    $spikeOut = Join-Path $OutDir "spike_$Tag.txt"
    if (Test-Path $spikeOut) { Remove-Item $spikeOut -Force }
    Get-Process -Name 'spike_t014_drop' -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 300

    $before = Get-LogLength
    $args = @("--mode=drag", "--payload=`"$Payload`"", "--to=$TargetX,$TargetY",
              "--hold-ms=6000", "--dwell-ms=$DwellMs", "--out=$spikeOut")
    $p = Start-Process -FilePath $SpikeExe -ArgumentList $args -PassThru
    [void]$p.WaitForExit(20000)
    if (-not $p.HasExited) { $p | Stop-Process -Force; "TIMEOUT" }
    Start-Sleep -Milliseconds 400
    $lines = Read-LogFrom $before
    return ($lines -split "`n" | Where-Object { $_ -match '\[dock\]' })
}

# The dock is restored to a known state first: every case asserts on the tile count,
# and a leftover tile from a previous run would make a rejection look like an add.
Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if (Test-Path "$configPath.t014bak") {
    Copy-Item "$configPath.t014bak" $configPath -Force
}
$env:DOCKFORGE_LOG_LEVEL = 'debug'
if ($Dpi -ne 192) { $env:DOCKFORGE_DPI_OVERRIDE = "$Dpi" } else { Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue }
$proc = Start-Process -FilePath $Exe -PassThru
Start-Sleep -Milliseconds $SettleMs

$rect = Get-DockRect
if (-not $rect) { throw "dock window not found" }
$centreX = [int](($rect.Left + $rect.Right) / 2)
# Aim at the middle of the plate, not the window: the top band is dead space and is
# excluded from the region while idle, so a drop there never reaches the target.
$plateY = [int]($rect.Bottom - ($rect.Bottom - $rect.Top) * 0.25)
$summary = @()
$summary += "dpi=$Dpi dock=$($rect.Left),$($rect.Top) $($rect.Right - $rect.Left)x$($rect.Bottom - $rect.Top) pid=$($proc.Id)"
$summary += "target=($centreX,$plateY)"

$lnk = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Cursor.lnk'
$doc = Join-Path $env:TEMP 'dockforge-drop-sample.txt'
$folder = Join-Path $env:TEMP 'dockforge-drop-folder'
if (-not (Test-Path $folder)) { New-Item -ItemType Directory -Force -Path $folder | Out-Null }

# 1. shortcut - accepted, becomes a tile
$lines = Invoke-Drag $lnk $centreX $plateY 'lnk'
$summary += '--- case=shortcut ---'
$summary += $lines
$cfg = [System.IO.File]::ReadAllText($configPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$summary += "after_shortcut tiles=$($cfg.dock.tiles.Count)"

# 2. same shortcut again - must move, not duplicate
$lines = Invoke-Drag $lnk $centreX $plateY 'lnk_again'
$summary += '--- case=shortcut_again ---'
$summary += $lines
$cfg = [System.IO.File]::ReadAllText($configPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$summary += "after_again tiles=$($cfg.dock.tiles.Count) (duplicate would be +1)"

# 3. document - rejected
$lines = Invoke-Drag $doc $centreX $plateY 'txt'
$summary += '--- case=document ---'
$summary += $lines
$cfg = [System.IO.File]::ReadAllText($configPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$summary += "after_document tiles=$($cfg.dock.tiles.Count) (rejection must not change it)"

# 4. folder - accepted
$lines = Invoke-Drag $folder $centreX $plateY 'folder'
$summary += '--- case=folder ---'
$summary += $lines
$cfg = [System.IO.File]::ReadAllText($configPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
$summary += "after_folder tiles=$($cfg.dock.tiles.Count)"
$summary += "folder_identity_present=$([bool]($cfg.dock.tiles | Where-Object { $_ -like '*dockforge-drop-folder*' }))"

$outPath = Join-Path $OutDir "drop_cases_dpi$Dpi.txt"
[System.IO.File]::WriteAllText($outPath, ($summary -join "`n"), (New-Object System.Text.UTF8Encoding($false)))
$summary -join "`n"
