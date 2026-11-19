# T-085 closing check: the production default index URL
# (raw.githubusercontent.com/chenmoulaile/DockForge/main/theme-index/index.json)
# is reachable AND the real app installs the pack it advertises - the same
# Themes-section flow as the probe's Part B, with no loopback origin.
$ErrorActionPreference = 'Stop'
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$Root = 'D:\MacDock'
$Scratch = Join-Path $env:TEMP ('dockforge-t085-real-' + [Environment]::TickCount)
foreach ($d in @((Join-Path $Scratch 'themes'), (Join-Path $Scratch 'config'), (Join-Path $Scratch 'logs'))) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
$env:DOCKFORGE_THEMES_DIR = Join-Path $Scratch 'themes'
$env:DOCKFORGE_CONFIG_PATH = Join-Path $Scratch 'config\config.json'
$env:DOCKFORGE_LOG_DIR = Join-Path $Scratch 'logs'
$CmdFile = Join-Path $Scratch 'settings_cmd.txt'
[System.IO.File]::WriteAllText($CmdFile, '', (New-Object System.Text.UTF8Encoding($false)))
$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_SHOW = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $CmdFile

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class R085 {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
}
"@
[void][R085]::SetProcessDPIAware()

# Selects row $Row of the $Index-th ListBox under the settings window
# (0 = community/remote, 1 = installed/local) - the same control a user clicks.
function Select-ListBox([int]$Index, [int]$Row) {
    $script:found = 0
    $script:target = [IntPtr]::Zero
    $settings = [R085]::FindWindowW('DockForgeSettingsWindow', 'DockForge Settings')
    if ($settings -eq [IntPtr]::Zero) { return $false }
    $cb = [R085+EnumProc]{
        param($h, $lp)
        $sb = New-Object System.Text.StringBuilder 256
        [void][R085]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'ListBox') {
            if ($script:found -eq $Index) { $script:target = $h }
            $script:found++
            if ($script:target -ne [IntPtr]::Zero) { return $false }
        }
        return $true
    }
    [void][R085]::EnumChildWindows($settings, $cb, [IntPtr]::Zero)
    if ($script:target -eq [IntPtr]::Zero) { return $false }
    [void][R085]::PostMessageW($script:target, 0x0186, [IntPtr]$Row, [IntPtr]::Zero)  # LB_SETCURSEL
    Start-Sleep -Milliseconds 120
    return $true
}

$report = New-Object System.Collections.Generic.List[string]
function Add-Report([string]$Line) { $script:report.Add($Line); Write-Output $Line }
function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($script:CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}
function Get-LatestLog {
    Get-ChildItem (Join-Path $Scratch 'logs') -Filter 'dockforge-*.log' |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
function Wait-LogMatch([long]$Offset, [string]$Pattern, [int]$TimeoutMs) {
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        $log = Get-LatestLog
        if ($log) {
            $fs = [System.IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
            try {
                $sr = New-Object System.IO.StreamReader($fs)
                $fs.Position = [Math]::Min($Offset, $fs.Length)
                $text = $sr.ReadToEnd(); $sr.Close()
                foreach ($line in ($text -split "`n")) {
                    if ($line -match $Pattern) { $fs.Close(); return $line }
                }
            } finally { if ($fs) { $fs.Close() } }
        }
        Start-Sleep -Milliseconds 80
    }
    return $null
}

$proc = Start-Process -FilePath (Join-Path $Root 'build\bin\DockForge.exe') -PassThru
$null = Wait-LogMatch 0 'probe file=' 20000
Start-Sleep -Milliseconds 400
Set-Command 'section themes'
Start-Sleep -Milliseconds 300
$offset = (Get-Item (Get-LatestLog).FullName).Length
Set-Command 'theme-refresh'
$line = Wait-LogMatch $offset 'theme browser: Index loaded' 30000
if ($line) { Add-Report "R1 ok: $line" } else { Add-Report 'R1 FAIL: default-URL index refresh never reported' }

if ($line) {
    $offset = (Get-Item (Get-LatestLog).FullName).Length
    $null = Select-ListBox -Index 0 -Row 0
    Set-Command 'theme-install'
    $line = Wait-LogMatch $offset "Installed 'mono'" 30000
    if ($line) { Add-Report "R2 ok: $line" } else { Add-Report 'R2 FAIL: default-URL install never reported' }
    $landed = Test-Path (Join-Path $env:DOCKFORGE_THEMES_DIR 'mono\theme.json')
    if ($landed) { Add-Report 'R3 ok: mono\theme.json landed from the default URL' }
    else { Add-Report 'R3 FAIL: pack not installed' }
}

Set-Command ''
$main = [R085]::FindWindowW('DockForgeHostWindow', $null)
if ($main -ne [IntPtr]::Zero) { [void][R085]::PostMessageW($main, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero) }
$proc = Get-Process -Name DockForge -ErrorAction SilentlyContinue
if ($proc -and -not $proc.WaitForExit(10000)) { Stop-Process -Id $proc.Id -Force }

$evidence = Join-Path $Root 'evidence\T-085\default_url_check.txt'
[System.IO.File]::WriteAllLines($evidence, $report, (New-Object System.Text.UTF8Encoding($false)))
$log = Get-LatestLog
if ($log) { Copy-Item $log.FullName (Join-Path $Root 'evidence\T-085\default_url_app_log.txt') -Force }
& (Join-Path $Root 'tools\probe_cleanup.ps1') -SkipTaskbar | Out-Null
Remove-Item Env:DOCKFORGE_THEMES_DIR -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_CONFIG_PATH -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_LOG_DIR -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS_SHOW -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS_CMD_FILE -ErrorAction SilentlyContinue
if ($report -match 'R3 ok') { exit 0 } else { exit 1 }
