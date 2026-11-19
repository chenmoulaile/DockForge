# DockForge T-085 evidence probe: community theme distribution.
#
# Part A - the production chain through theme_dist_probe.exe against a loopback
#          HttpListener origin (index fetch -> SHA256 -> install -> loader ->
#          tamper refusal -> executable-content refusal -> uninstall).
# Part B - the real app: settings window Themes section driven over the
#          DOCKFORGE_SETTINGS_CMD_FILE channel (theme-refresh -> theme-install
#          -> set appearance.theme -> theme-disable -> theme-delete), every
#          step verified from the app's own log lines, screenshots at the end.
#
# Everything runs on scratch dirs (DOCKFORGE_THEMES_DIR / DOCKFORGE_CONFIG_PATH /
# DOCKFORGE_LOG_DIR), so neither the real profile nor the shell is touched.
param(
    [string]$OutDir = 'evidence\T-085',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$ProbeExe = 'build\bin\theme_dist_probe.exe'
)
$ErrorActionPreference = 'Stop'
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

$RepoRoot = (Get-Location).Path
$Evidence = Join-Path $RepoRoot $OutDir
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null
$Scratch = Join-Path $env:TEMP ('dockforge-t085-' + [Environment]::TickCount)
$ThemesDir = Join-Path $Scratch 'themes'
$ConfigDir = Join-Path $Scratch 'config'
$LogDir = Join-Path $Scratch 'logs'
foreach ($d in @($ThemesDir, $ConfigDir, $LogDir)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
$env:DOCKFORGE_THEMES_DIR = $ThemesDir
$env:DOCKFORGE_CONFIG_PATH = Join-Path $ConfigDir 'config.json'
$env:DOCKFORGE_LOG_DIR = $LogDir

$CmdFile = Join-Path $Scratch 'settings_cmd.txt'
[System.IO.File]::WriteAllText($CmdFile, '', (New-Object System.Text.UTF8Encoding($false)))

$report = New-Object System.Collections.Generic.List[string]
$failures = 0
function Add-Report([string]$Line) { $script:report.Add($Line); Write-Output $Line }
function Add-Fail([string]$Line) { $script:failures++; Add-Report "FAIL: $Line" }
function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($script:CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}
function Get-LatestLog {
    Get-ChildItem $LogDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
function Wait-LogMatch([long]$Offset, [string]$Pattern, [int]$TimeoutMs = 10000) {
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
        Start-Sleep -Milliseconds 60
    }
    return $null
}
function Save-SettingsShot([string]$Path) {
    Add-Type -AssemblyName System.Drawing
    $hwnd = [T085Probe]::FindWindowW('DockForgeSettingsWindow', 'DockForge Settings')
    if ($hwnd -eq [IntPtr]::Zero) { Add-Fail "no settings window for $Path"; return $false }
    $r = New-Object T085Probe+RECT
    [void][T085Probe]::GetWindowRect($hwnd, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.Right - $r.Left), ($r.Bottom - $r.Top)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    [void][T085Probe]::PrintWindow($hwnd, $hdc, 2)  # PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return $true
}

# Win32 helpers, defined once (Add-Type twice with the same name throws).
Add-Type -AssemblyName System.Drawing
if (-not ([System.Management.Automation.PSTypeName]'T085Probe').Type) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class T085Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
}
[void][T085Probe]::SetProcessDPIAware()
if (-not ([System.Management.Automation.PSTypeName]'T085Close').Type) {
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class T085Close {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, System.Text.StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lp);
}
"@
}

# Selects row $Row of the $Index-th ListBox under the settings window
# (creation order: 0 = community/remote list, 1 = installed/local list). This
# is the selection a user makes by clicking, via the same control.
function Select-ListBox([int]$Index, [int]$Row) {
    $script:found = 0
    $script:target = [IntPtr]::Zero
    $settings = [T085Probe]::FindWindowW('DockForgeSettingsWindow', 'DockForge Settings')
    if ($settings -eq [IntPtr]::Zero) { Add-Fail 'Select-ListBox: no settings window'; return }
    $cb = [T085Close+EnumProc]{
        param($h, $lp)
        $sb = New-Object System.Text.StringBuilder 256
        [void][T085Close]::GetClassNameW($h, $sb, 256)
        if ($sb.ToString() -eq 'ListBox') {
            if ($script:found -eq $Index) { $script:target = $h }
            $script:found++
            if ($script:target -ne [IntPtr]::Zero) { return $false }
        }
        return $true
    }
    [void][T085Close]::EnumChildWindows($settings, $cb, [IntPtr]::Zero)
    if ($script:target -eq [IntPtr]::Zero) { Add-Fail "Select-ListBox: ListBox #$Index not found"; return }
    # LB_SETCURSEL = 0x0186
    [void][T085Close]::PostMessageW($script:target, 0x0186, [IntPtr]$Row, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 100
}

# ---- build the packs (Compress-Archive; the .dockforge-theme name is a copy) -
Add-Report "== T-085 theme distribution probe $(Get-Date -Format o) =="
Add-Report "scratch=$Scratch"

$PayloadDir = Join-Path $Scratch 'payload-mono'
New-Item -ItemType Directory -Force -Path (Join-Path $PayloadDir 'icons') | Out-Null
[System.IO.File]::WriteAllText((Join-Path $PayloadDir 'theme.json'), @'
{ "name": "mono", "author": "dockforge", "version": "1.0.0",
  "overrides": { "appearance": { "corner_radius": 12 } } }
'@, (New-Object System.Text.UTF8Encoding($false)))
# A real 512x288 PNG preview, drawn on the fly (no binary fixture in the repo).
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap 512, 288
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::FromArgb(28, 28, 32))
$g.FillEllipse([System.Drawing.Brushes]::White, 196, 104, 120, 80)
$g.Dispose()
$bmp.Save((Join-Path $PayloadDir 'preview.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
[System.IO.File]::WriteAllText((Join-Path $PayloadDir 'icons\sample.png'), 'icon-bytes',
    (New-Object System.Text.UTF8Encoding($false)))

$ZipTmp = Join-Path $Scratch 'mono.zip'
$GoodPack = Join-Path $Scratch 'mono.dockforge-theme'
Compress-Archive -Path (Join-Path $PayloadDir '*') -DestinationPath $ZipTmp -CompressionLevel Optimal
Copy-Item $ZipTmp $GoodPack
$GoodSha = (Get-FileHash $GoodPack -Algorithm SHA256).Hash.ToLowerInvariant()
$GoodSize = (Get-Item $GoodPack).Length

# Tampered pack: different bytes, same target name in the install request.
$OtherPack = Join-Path $Scratch 'other.dockforge-theme'
[System.IO.File]::WriteAllText((Join-Path $PayloadDir 'theme.json'), @'
{ "name": "mono", "author": "dockforge", "version": "9.9.9",
  "overrides": { "appearance": { "corner_radius": 40 } } }
'@, (New-Object System.Text.UTF8Encoding($false)))
$ZipTmp2 = Join-Path $Scratch 'other.zip'
Compress-Archive -Path (Join-Path $PayloadDir '*') -DestinationPath $ZipTmp2 -CompressionLevel Optimal
Copy-Item $ZipTmp2 $OtherPack

# Executable-content pack: passes any SHA check, must fail the C-02 validator.
$ExecDir = Join-Path $Scratch 'payload-exec'
New-Item -ItemType Directory -Force -Path $ExecDir | Out-Null
[System.IO.File]::WriteAllText((Join-Path $ExecDir 'theme.json'), '{}',
    (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText((Join-Path $ExecDir 'helper.exe'), 'MZ fake',
    (New-Object System.Text.UTF8Encoding($false)))
$ZipTmp3 = Join-Path $Scratch 'exec.zip'
$ExecPack = Join-Path $Scratch 'exec.dockforge-theme'
Compress-Archive -Path (Join-Path $ExecDir '*') -DestinationPath $ZipTmp3 -CompressionLevel Optimal
Copy-Item $ZipTmp3 $ExecPack

$IndexJson = @'
{
  "schema": 1,
  "updated": "__STAMP__",
  "themes": [ {
    "name": "mono",
    "author": "dockforge",
    "version": "1.0.0",
    "preview_url": "__ORIGIN__/theme-index/assets/mono-preview.png",
    "download_url": "__ORIGIN__/theme-index/packs/mono.dockforge-theme",
    "sha256": "__SHA__",
    "size_bytes": __SIZE__
  } ]
}
'@
$Port = Get-Random -Minimum 19000 -Maximum 19999
$Origin = "http://127.0.0.1:$Port"
$IndexJson = $IndexJson.Replace('__STAMP__', (Get-Date -Format 'yyyy-MM-dd')).Replace('__ORIGIN__', $Origin).Replace('__SHA__', $GoodSha).Replace('__SIZE__', "$GoodSize")
$IndexFile = Join-Path $Scratch 'index.json'
[System.IO.File]::WriteAllText($IndexFile, $IndexJson, (New-Object System.Text.UTF8Encoding($false)))
Add-Report "origin=$Origin pack=$GoodPack sha=$GoodSha size=$GoodSize"

# ---- the loopback origin (job owns the listener exclusively; see spike) ------
$ServerJob = Start-Job -ScriptBlock {
    param($Prefix, $IndexFile, $GoodPack, $OtherPack, $ExecPack)
    $l = New-Object System.Net.HttpListener
    $l.Prefixes.Add($Prefix)
    $l.Start()
    $map = @{
        '/theme-index/index.json'              = $IndexFile
        '/theme-index/packs/mono.dockforge-theme'  = $GoodPack
        '/theme-index/packs/other.dockforge-theme' = $OtherPack
        '/theme-index/packs/exec.dockforge-theme'  = $ExecPack
    }
    while ($l.IsListening) {
        try {
            $ctx = $l.GetContext()
            $path = $ctx.Request.Url.AbsolutePath
            if ($map.ContainsKey($path)) {
                $bytes = [System.IO.File]::ReadAllBytes($map[$path])
                $ctx.Response.ContentType = 'application/octet-stream'
                $ctx.Response.ContentLength64 = $bytes.Length
                $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
                $ctx.Response.StatusCode = 200
            } else {
                $ctx.Response.StatusCode = 404
            }
            $ctx.Response.OutputStream.Close()
        } catch { break }
    }
} -ArgumentList "$Origin/", $IndexFile, $GoodPack, $OtherPack, $ExecPack

$Ready = $false
for ($i = 0; $i -lt 60 -and -not $Ready; ++$i) {
    Start-Sleep -Milliseconds 250
    try {
        [System.Net.WebClient]::new().DownloadString("$Origin/ready") | Out-Null
        $Ready = $true
    } catch {
        $resp = $_.Exception.InnerException.Response
        if ($resp -eq $null) { $resp = $_.Exception.Response }
        if ($resp -ne $null -and [int]$resp.StatusCode -eq 404) { $Ready = $true }
    }
}
if (-not $Ready) { Add-Fail 'origin never came up'; Receive-Job $ServerJob -Keep | Out-String | Add-Report }
else { Add-Report 'A0 origin ready' }

# ---- Part A: production chain ------------------------------------------------
Add-Report '--- Part A: theme_dist_probe.exe (production modules) ---'
$probeOut = & $ProbeExe $Port 2>&1
Add-Report ($probeOut | Out-String).TrimEnd()
if ($LASTEXITCODE -ne 0) { Add-Fail "Part A: theme_dist_probe.exe rc=$LASTEXITCODE" }

# ---- Part B: the real app, settings Themes section ---------------------------
Add-Report '--- Part B: DockForge.exe settings Themes section ---'
# The app reads themes.index_url from config.json; point it at the loopback
# origin (the default URL targets the not-yet-published upstream index).
[System.IO.File]::WriteAllText($env:DOCKFORGE_CONFIG_PATH,
    '{ "themes": { "index_url": "' + $Origin + '/theme-index/index.json" } }',
    (New-Object System.Text.UTF8Encoding($false)))
Set-Command ''
$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_SHOW = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $CmdFile
$env:DOCKFORGE_SETTINGS_THEME = 'light'
$proc = Start-Process -FilePath (Join-Path $RepoRoot $Exe) -PassThru
try {
    $null = Wait-LogMatch 0 'settings window created' 15000
    # The command channel is armed at 'probe file=' - anything written
    # before it is Prime-marked as already seen (T-076 stale-command
    # protection) and silently ignored, measured in this probe's first run.
    $null = Wait-LogMatch 0 'probe file=' 15000
    Start-Sleep -Milliseconds 400

    Set-Command 'section themes'
    Start-Sleep -Milliseconds 400

    $offset = (Get-Item (Get-LatestLog).FullName).Length
    Set-Command 'theme-refresh'
    $line = Wait-LogMatch $offset 'theme browser: Index loaded: 1 theme' 20000
    if ($line) { Add-Report "B1 refresh ok: $line" } else { Add-Fail 'B1: index refresh never reported' }

    $offset = (Get-Item (Get-LatestLog).FullName).Length
    Select-ListBox -Index 0 -Row 0
    Set-Command 'theme-install'
    $line = Wait-LogMatch $offset "theme browser: Installed 'mono'" 30000
    if ($line) { Add-Report "B2 install ok: $line" } else { Add-Fail 'B2: install never reported' }
    $installed = Test-Path (Join-Path $ThemesDir 'mono\theme.json')
    if ($installed) { Add-Report 'B3 pack landed in themes dir (theme.json present)' }
    else { Add-Fail 'B3: mono\theme.json missing after app-side install' }

    # Enable from the local manager. The probe pre-selects the 'mono' row of
    # the installed list (row 2: default, dark, mono) with LB_SETCURSEL - the
    # keyboard/click selection a user would make, then theme-enable runs the
    # same RecordEdit chain as the Appearance combo.
    $offset = (Get-Item (Get-LatestLog).FullName).Length
    Select-ListBox -Index 1 -Row 2
    Set-Command 'theme-enable'
    $line = Wait-LogMatch $offset 'theme browser: enable mono via RecordEdit' 10000
    if ($line) { Add-Report "B4 enable ok: $line" } else { Add-Fail 'B4: enable never reported' }
    $line = Wait-LogMatch $offset 'flush applied' 10000
    if ($line) { Add-Report "B4 flush: $line" } else { Add-Fail 'B4: flush never ran' }
    $cfgText = [System.IO.File]::ReadAllText($env:DOCKFORGE_CONFIG_PATH)
    if ($cfgText -match '"theme":\s*"mono"') { Add-Report 'B4 config.json: appearance.theme = mono persisted' }
    else { Add-Fail 'B4: config.json did not persist theme=mono' }

    Add-Report 'B5 screenshot (light, themes section)'
    Start-Sleep -Milliseconds 300
    $null = Save-SettingsShot (Join-Path $Evidence 'themes_section_light.png')

    # Disable back to default.
    $offset = (Get-Item (Get-LatestLog).FullName).Length
    Set-Command 'theme-disable'
    $line = Wait-LogMatch $offset 'theme browser: disable, back to default' 10000
    if ($line) { Add-Report "B6 disable ok: $line" } else { Add-Fail 'B6: disable never reported' }
    $line = Wait-LogMatch $offset 'flush applied' 10000
    $cfgText = [System.IO.File]::ReadAllText($env:DOCKFORGE_CONFIG_PATH)
    if ($cfgText -match '"theme":\s*"default"') { Add-Report 'B6 config.json: back to default' }
    else { Add-Fail 'B6: config.json did not revert to default' }

    # Delete the installed pack (select row 2 again; delete needs a selection).
    $offset = (Get-Item (Get-LatestLog).FullName).Length
    Select-ListBox -Index 1 -Row 2
    Set-Command 'theme-delete'
    $line = Wait-LogMatch $offset "uninstalled theme 'mono'" 10000
    if ($line) { Add-Report "B7 delete ok: $line" } else { Add-Fail 'B7: delete never reported' }
    if (-not (Test-Path (Join-Path $ThemesDir 'mono'))) { Add-Report 'B7 pack directory removed' }
    else { Add-Fail 'B7: mono directory still present after delete' }

    # Open themes folder (log line is the verdict; a real Explorer window opens).
    $offset = (Get-Item (Get-LatestLog).FullName).Length
    Set-Command 'theme-open-folder'
    $line = Wait-LogMatch $offset 'opened themes folder' 10000
    if ($line) { Add-Report "B8 open folder ok: $line" } else { Add-Fail 'B8: open folder never reported' }

    # Graceful close: post WM_CLOSE to the main window so the exit path runs
    # (T085Close was defined once above, next to the capture helpers).
    $main = [T085Close]::FindWindowW('DockForgeHostWindow', $null)
    if ($main -ne [IntPtr]::Zero) {
        [void][T085Close]::PostMessageW($main, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero)
    }
    if (-not $proc.WaitForExit(10000)) { Stop-Process -Id $proc.Id -Force }
} finally {
    Set-Command ''
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}

# ---- B9: dark palette screenshot (fresh process: PreferLightTheme reads the
# env at paint time, so the override must be in the app's environment) --------
$env:DOCKFORGE_SETTINGS_SHOW = '1'
$env:DOCKFORGE_SETTINGS_THEME = 'dark'
# Wait from the log length NOW: offset 0 would instantly match the first
# app's 'settings window created' line and race the second process.
$darkOffset = (Get-Item (Get-LatestLog).FullName).Length
$proc2 = Start-Process -FilePath (Join-Path $RepoRoot $Exe) -PassThru
try {
    $null = Wait-LogMatch $darkOffset 'settings window created' 15000
    $null = Wait-LogMatch $darkOffset 'probe file=' 15000
    Start-Sleep -Milliseconds 400
    Set-Command 'section themes'
    Start-Sleep -Milliseconds 400
    Start-Sleep -Milliseconds 400
    Add-Report 'B9 screenshot (dark, themes section)'
    $null = Save-SettingsShot (Join-Path $Evidence 'themes_section_dark.png')
} finally {
    $main2 = [T085Close]::FindWindowW('DockForgeHostWindow', $null)
    if ($main2 -ne [IntPtr]::Zero) {
        [void][T085Close]::PostMessageW($main2, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero)
    }
    if (-not $proc2.WaitForExit(10000)) { Stop-Process -Id $proc2.Id -Force }
}
$env:DOCKFORGE_SETTINGS_THEME = $null

# ---- cleanup ------------------------------------------------------------------
Stop-Job $ServerJob -ErrorAction SilentlyContinue
Remove-Job $ServerJob -Force -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS_SHOW -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS_CMD_FILE -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_SETTINGS_THEME -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_THEMES_DIR -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_CONFIG_PATH -ErrorAction SilentlyContinue
Remove-Item Env:DOCKFORGE_LOG_DIR -ErrorAction SilentlyContinue
# Keep the app log for the evidence bundle before anything is removed.
$appLog = Get-LatestLog
if ($appLog) { Copy-Item $appLog.FullName (Join-Path $Evidence 'app_log.txt') -Force }
& (Join-Path $RepoRoot 'tools\probe_cleanup.ps1') -SkipTaskbar | Out-Null
Remove-Item -Recurse -Force $Scratch -ErrorAction SilentlyContinue

$verdict = if ($failures -eq 0) { 'VERDICT=PASS' } else { "VERDICT=FAIL ($failures)" }
Add-Report $verdict
[System.IO.File]::WriteAllLines((Join-Path $Evidence 'probe.txt'), $report,
    (New-Object System.Text.UTF8Encoding($false)))
if ($failures -gt 0) { exit 1 } else { exit 0 }
