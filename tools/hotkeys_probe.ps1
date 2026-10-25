# DockForge T-069 evidence probe: hotkeys + screen hotspots, live.
#
# Log-driven shutter pattern (same as every other probe): PASS/FAIL comes from
# the app's own log lines plus real Win32 state (IsWindowVisible of the dock,
# RegisterHotKey results from THIS process), never from the eye. Screenshots
# document the visuals.
#
# Chain under test (T-069 DoD + C-24). Everything runs config-isolated
# (DOCKFORGE_CONFIG_PATH + DOCKFORGE_LOG_DIR under %TEMP%); the physical mouse
# is not touched except by SetCursorPos (Part F), the keyboard only receives
# injected keybd_event chords (Part C), and no machine state beyond the isolated
# config file is modified.
#
#   A  fresh install registers nothing: log "applied 0 hotkey entries",
#      config.json carries no hotkeys/hotspots key, and THIS process can
#      register the catalog combos (proves the app holds none of them).
#   B  settings checkbox on (probe `set hotkeys.toggle_dock.enabled 1` ->
#      RecordEdit path): log "applied 1 hotkey entries, 1 registered",
#      config.json gains the C-24 entry, and registering Ctrl+Alt+D here now
#      fails with 1409 (the app holds it).
#   C  end-to-end hotkey: keybd_event Ctrl+Alt+D -> WM_HOTKEY -> action fires
#      -> dock hides; second press -> dock shows again.
#   D  conflict: this process pre-registers Ctrl+Alt+K, then a config.json
#      edit (file + hot reload) enables a C-24 entry on that combo -> WARN
#      "probe.clash ... 已被其他程序占用 (err=1409)".
#   E  exit no-residue: WM_CLOSE the app, wait for process exit, then
#      registering Ctrl+Alt+D here SUCCEEDS (the app unregistered on exit).
#   F  hotspot: config enables a 12px top-left corner (action toggle_dock);
#      SetCursorPos into the corner -> "hotspot 'probe.corner' entered" +
#      dock hides; cursor to centre (re-arm), back into the corner -> fires
#      again and the dock shows.
#   G  CPU with hotspots on vs off: Get-Process CPU over 8s each way,
#      reported (the C-24 ceiling argument; the spike measured the raw loop).
#   H  screenshot: settings window Shortcuts section (checkbox + status line).
param(
    [string]$OutDir = 'evidence\T-069',
    [string]$Exe = 'build\bin\DockForge.exe'
)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

function Fail([string]$why) {
    Write-Host "FAIL: $why"
    Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
    exit 1
}

Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
if (-not (Test-Path $Exe)) { Fail "app missing: $Exe" }

$scratch = Join-Path $env:TEMP ('dockforge-t069-' + [Environment]::TickCount)
New-Item -ItemType Directory -Force -Path $scratch | Out-Null
$configPath = Join-Path $scratch 'config.json'
$logDir = Join-Path $scratch 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
'{}' | Set-Content -Path $configPath -Encoding ASCII

$cmdFile = Join-Path $scratch 'settings-cmd.txt'
New-Item -ItemType File -Force -Path $cmdFile | Out-Null

$env:DOCKFORGE_CONFIG_PATH = $configPath
$env:DOCKFORGE_LOG_DIR = $logDir
$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $cmdFile
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'

function Latest-Log {
    Get-ChildItem $logDir -Filter 'dockforge-*.log' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

function Wait-Log([string]$Pattern, [int]$FromLength, [int]$TimeoutMs = 8000) {
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
        $log = Latest-Log
        if ($log) {
            # -Encoding UTF8: the log carries Chinese text (the C-24 conflict
            # wording); PS 5.1's default ANSI read turns it into mojibake and
            # the pattern would never match (measured on the first probe run).
            $text = Get-Content $log.FullName -Raw -Encoding UTF8
            if ($text.Length -gt $FromLength) {
                $fresh = $text.Substring($FromLength)
                $m = [regex]::Match($fresh, $Pattern)
                if ($m.Success) { return $m.Value }
            }
        }
        Start-Sleep -Milliseconds 100
    }
    return $null
}

function Send-Cmd([string]$cmd) {
    # The settings channel treats the WHOLE file as one command line (it has
    # no multi-line token convention like the hw panel's), and the app ignores
    # byte-identical repeats - every command this probe sends is unique, so a
    # plain write is correct.
    $cmd | Set-Content -Path $cmdFile -Encoding ASCII
}

# Writes config.json (no BOM; the loader rejects a BOM'd file as invalid JSON).
function Write-ConfigJson([string]$json) {
    [IO.File]::WriteAllText($configPath, $json, (New-Object System.Text.UTF8Encoding($false)))
}

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T069Probe {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr v);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool RegisterHotKey(IntPtr h, int id, uint mods, uint vk);
    [DllImport("user32.dll")] public static extern bool UnregisterHotKey(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    public static IntPtr Find(string cls, bool needVisible) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            var c = new StringBuilder(256);
            GetClassNameW(h, c, 256);
            if (c.ToString() == cls && (!needVisible || IsWindowVisible(h))) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    // Injects a Ctrl+Alt+<vk> chord. Same API and order as the T-061 taskbar
    // probe (evidence: injected input reaches RegisterHotKey hotkeys).
    public static void SendChord(byte vk) {
        keybd_event(0x11, 0, 0, UIntPtr.Zero);            // Ctrl down
        keybd_event(0x12, 0, 0, UIntPtr.Zero);            // Alt down
        keybd_event(vk, 0, 0, UIntPtr.Zero);
        keybd_event(vk, 0, 2, UIntPtr.Zero);              // KEYEVENTF_KEYUP
        keybd_event(0x12, 0, 2, UIntPtr.Zero);
        keybd_event(0x11, 0, 2, UIntPtr.Zero);
    }
}
'@
Add-Type -AssemblyName System.Drawing
[T069Probe]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null

$MOD_CTRL_ALT = 3
$VK_D = 0x44; $VK_K = 0x4B

$report = @()
function Add-Report([string]$line) { $script:report += $line; Write-Host "  $line" }
function Add-Fail([string]$line) { $script:report += "FAIL: $line"; Write-Host "  FAIL: $line" }

Write-Host "=== T-069 probe: scratch=$scratch ==="

# ---- launch ----------------------------------------------------------------
Start-Process -FilePath (Join-Path $root $Exe) -WorkingDirectory $root
Start-Sleep -Seconds 3
$log = Latest-Log
if (-not $log) { Fail 'no log file written' }
$len = (Get-Item $log.FullName).Length

# ---- A: defaults register nothing ------------------------------------------
Write-Host "`n[A] fresh install registers nothing (C-24 defaults off)"
# The apply line is written during startup - often before the 3s sleep below -
# so this part searches the WHOLE log, not the增量 since launch.
$line = Wait-Log 'applied \d+ hotkey entries, \d+ registered, \d+ failed' 0
if ($line -and $line -match 'applied 0 hotkey entries, 0 registered, 0 failed') {
    Add-Report "A ok: '$line' (nothing registered at startup)"
} else {
    Add-Fail "A: expected 'applied 0 hotkey entries, 0 registered, 0 failed', got '$line'"
}
$configText = Get-Content $configPath -Raw
if ($configText -notmatch '"hotkeys"' -and $configText -notmatch '"hotspots"') {
    Add-Report 'A ok: default config.json carries no hotkeys/hotspots key'
} else {
    Add-Fail 'A: default config.json unexpectedly mentions hotkeys/hotspots'
}
# Whether the catalog combos are free is machine state (another program may
# legitimately hold one - that is exactly what conflict detection is for), so
# this is reported, not asserted. The app holding NONE of them is already
# proven by the "applied 0" log line above.
$catalogCombos = @(
    @{vk = 0x44; name = 'Ctrl+Alt+D'},   # toggle_dock
    @{vk = 0x4C; name = 'Ctrl+Alt+L'},   # toggle_launchpad
    @{vk = 0x42; name = 'Ctrl+Alt+B'},   # toggle_taskbar
    @{vk = 0x50; name = 'Ctrl+Alt+P'},   # cycle_dock_side
    @{vk = 0x47; name = 'Ctrl+Alt+G'},   # toggle_magnify
    @{vk = 0x4D; name = 'Ctrl+Alt+M'}    # minimize_all
)
$free = 0
$busy = @()
for ($i = 0; $i -lt $catalogCombos.Count; ++$i) {
    if ([T069Probe]::RegisterHotKey([IntPtr]::Zero, 9000 + $i, $MOD_CTRL_ALT, $catalogCombos[$i].vk)) {
        $free++
        [T069Probe]::UnregisterHotKey([IntPtr]::Zero, 9000 + $i) | Out-Null
    } else {
        $busy += $catalogCombos[$i].name
    }
}
Add-Report ("A info: {0}/{1} catalog combos free on this machine (held by other programs: {2})" -f `
    $free, $catalogCombos.Count, ($(if ($busy) { $busy -join ', ' } else { 'none' })))

# ---- B: settings checkbox -> registration ----------------------------------
Write-Host "`n[B] settings checkbox enables the hotkey (RecordEdit path)"
$len = (Get-Item (Latest-Log).FullName).Length
Send-Cmd 'set hotkeys.toggle_dock.enabled 1'
$line = Wait-Log 'applied \d+ hotkey entries, \d+ registered(, \d+ failed)?' $len
if ($line -and $line -match 'applied 1 hotkey entries, 1 registered') {
    Add-Report "B ok: '$line' after the checkbox flip"
} else {
    Add-Fail "B: expected 'applied 1 hotkey entries, 1 registered', got '$line'"
}
$configText = Get-Content $configPath -Raw
if ($configText -match '"hotkeys"' -and $configText -match '"toggle_dock"') {
    Add-Report 'B ok: config.json persisted the C-24 hotkeys array'
} else {
    Add-Fail 'B: config.json has no hotkeys array after the checkbox flip'
}
if (-not [T069Probe]::RegisterHotKey([IntPtr]::Zero, 9100, $MOD_CTRL_ALT, $VK_D)) {
    $err = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
    if ($err -eq 1409) { Add-Report 'B ok: Ctrl+Alt+D now 1409 here (the app holds it)' }
    else { Add-Fail "B: Ctrl+Alt+D register failed with unexpected err=$err" }
} else {
    [T069Probe]::UnregisterHotKey([IntPtr]::Zero, 9100) | Out-Null
    Add-Fail 'B: Ctrl+Alt+D still free - the app did not register it'
}

# ---- C: end-to-end hotkey ----------------------------------------------------
Write-Host "`n[C] injected Ctrl+Alt+D toggles the dock"
$dock = [T069Probe]::Find('DockForgeHostWindow', $true)
if ($dock -eq [IntPtr]::Zero) { Fail 'C: no visible dock window found' }
$len = (Get-Item (Latest-Log).FullName).Length
[T069Probe]::SendChord($VK_D)
$line = Wait-Log "hotkey 'toggle_dock' -> toggle_dock" $len
if ($line) { Add-Report "C ok: '$line' (WM_HOTKEY dispatched)" }
else { Add-Fail 'C: no dispatch log for the first Ctrl+Alt+D' }
$line = Wait-Log 'action toggle_dock \(0\) fired' $len
if ($line) { Add-Report 'C ok: action fired (category 0 = ComponentToggle)' }
else { Add-Fail 'C: action never fired for the first press' }
Start-Sleep -Milliseconds 500
if (-not [T069Probe]::IsWindowVisible($dock)) {
    Add-Report 'C ok: dock HIDDEN after the first press'
} else {
    Add-Fail 'C: dock still visible after the first press'
}
$len = (Get-Item (Latest-Log).FullName).Length
[T069Probe]::SendChord($VK_D)
$line = Wait-Log 'action toggle_dock \(0\) fired' $len
Start-Sleep -Milliseconds 500
if ([T069Probe]::IsWindowVisible($dock)) {
    Add-Report 'C ok: dock VISIBLE again after the second press'
} else {
    Add-Fail 'C: dock did not come back after the second press'
}

# ---- C2: DisplayMode category (toggle_magnify, Ctrl+Alt+G) -------------------
Write-Host "`n[C2] injected Ctrl+Alt+G toggles magnification (DisplayMode)"
Send-Cmd 'set hotkeys.toggle_magnify.enabled 1'
$line = Wait-Log 'applied 2 hotkey entries, 2 registered, 0 failed' (Get-Item (Latest-Log).FullName).Length
if (-not $line) { Add-Fail 'C2: magnify hotkey did not register (2 entries expected)' }
$len = (Get-Item (Latest-Log).FullName).Length
[T069Probe]::SendChord(0x47)  # 'G'
$line = Wait-Log 'action toggle_magnify \(2\) fired' $len
if ($line) { Add-Report 'C2 ok: action fired (category 2 = DisplayMode)' }
else { Add-Fail 'C2: toggle_magnify never fired' }
$cfg = (Get-Content $configPath -Raw) | ConvertFrom-Json
if ([double]$cfg.dock.magnification -eq 1.0) {
    Add-Report 'C2 ok: config.json magnification=1.0 (1.8 default toggled off)'
} else {
    Add-Fail ("C2: magnification unexpected: " + $cfg.dock.magnification)
}

# ---- C3: SystemControl category (toggle_taskbar, Ctrl+Alt+B) -----------------
Write-Host "`n[C3] injected Ctrl+Alt+B toggles the taskbar (SystemControl)"
Send-Cmd 'set hotkeys.toggle_taskbar.enabled 1'
$line = Wait-Log 'applied 3 hotkey entries, 3 registered, 0 failed' (Get-Item (Latest-Log).FullName).Length
if (-not $line) { Add-Fail 'C3: taskbar hotkey did not register (3 entries expected)' }
$len = (Get-Item (Latest-Log).FullName).Length
[T069Probe]::SendChord(0x42)  # 'B'
$line = Wait-Log 'action toggle_taskbar \(1\) fired' $len
if ($line) { Add-Report 'C3 ok: action fired (category 1 = SystemControl enum)' }
else { Add-Fail 'C3: toggle_taskbar never fired' }
$line = Wait-Log 'hide_taskbar switch -> 1: ok' $len
if ($line) { Add-Report "C3 ok: '$line' (the T-061 apply path ran)" }
else { Add-Fail 'C3: hide_taskbar switch never applied' }
# Restore immediately: keep the probe's footprint on the desktop at zero.
Send-Cmd 'set hotkeys.toggle_taskbar.enabled 0'
Send-Cmd 'set behavior.hide_taskbar 0'
$null = Wait-Log 'hide_taskbar switch -> 0: ok' (Get-Item (Latest-Log).FullName).Length

# ---- D: conflict via hot reload ---------------------------------------------
Write-Host "`n[D] a combo another program holds reports 1409"
if (-not [T069Probe]::RegisterHotKey([IntPtr]::Zero, 9200, $MOD_CTRL_ALT, $VK_K)) {
    Fail 'D: probe could not pre-register Ctrl+Alt+K'
}
$configText = Get-Content $configPath -Raw
$cfg = $configText | ConvertFrom-Json
$clash = @{
    id = 'probe.clash'; modifiers = 3; vk = 75; action = 'toggle_dock'; enabled = $true
}
# PSCustomObject: a missing property cannot be assigned directly, so collect
# and re-attach through Add-Member -Force (works for both missing and present).
$existing = @()
if ($cfg.PSObject.Properties['hotkeys']) { $existing = @($cfg.hotkeys) }
$cfg | Add-Member -NotePropertyName hotkeys -NotePropertyValue ($existing + $clash) -Force
$json = $cfg | ConvertTo-Json -Depth 8
Write-ConfigJson $json
$len = (Get-Item (Latest-Log).FullName).Length
$line = Wait-Log "probe\.clash.*已被其他程序占用" $len 10000
if ($line) { Add-Report "D ok: conflict surfaced -> '$line'" }
else { Add-Fail 'D: no conflict WARN for probe.clash after the hot reload' }
$line2 = Wait-Log 'applied \d+ hotkey entries, \d+ registered, \d+ failed' $len 10000
if ($line2 -and $line2 -match 'applied 4 hotkey entries, 3 registered, 1 failed') {
    Add-Report "D ok: '$line2' (3 good + 1 conflict)"
} else {
    Add-Fail "D: unexpected apply summary: '$line2'"
}

# ---- H: settings Shortcuts screenshot, taken WHILE the conflict is live -----
Write-Host "`n[H] settings Shortcuts section screenshot (conflict status line visible)"
Send-Cmd 'show'
Start-Sleep -Milliseconds 600
Send-Cmd 'section shortcuts'
Start-Sleep -Milliseconds 900
Add-Type -AssemblyName System.Windows.Forms
$settingsHwnd = [T069Probe]::Find('DockForgeSettingsWindow', $true)
if ($settingsHwnd -ne [IntPtr]::Zero) {
    $rect = New-Object T069Probe+RECT
    [T069Probe]::GetWindowRect($settingsHwnd, [ref]$rect) | Out-Null
    $w = $rect.Right - $rect.Left; $ht = $rect.Bottom - $rect.Top
    if ($w -gt 0 -and $ht -gt 0) {
        $bmp = New-Object System.Drawing.Bitmap($w, $ht)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($rect.Left, $rect.Top, 0, 0, (New-Object System.Drawing.Size($w, $ht)))
        $g.Dispose()
        $bmp.Save((Join-Path $OutDir 'settings_shortcuts.png'), [System.Drawing.Imaging.ImageFormat]::Png)
        $bmp.Dispose()
        Add-Report 'H ok: settings_shortcuts.png saved (with the conflict line)'
    } else { Add-Fail 'H: settings window rect empty' }
} else {
    Add-Fail 'H: settings window not visible for the screenshot'
}
Send-Cmd 'hide'

[T069Probe]::UnregisterHotKey([IntPtr]::Zero, 9200) | Out-Null
# Clean the clash entry back out for the remaining parts.
$cfg = (Get-Content $configPath -Raw) | ConvertFrom-Json
$kept = @($cfg.hotkeys | Where-Object { $_.id -ne 'probe.clash' })
$cfg | Add-Member -NotePropertyName hotkeys -NotePropertyValue $kept -Force
Write-ConfigJson ($cfg | ConvertTo-Json -Depth 8)
$null = Wait-Log 'applied 1 hotkey entries, 1 registered, 0 failed' (Get-Item (Latest-Log).FullName).Length 10000

# ---- E: exit unregisters everything -----------------------------------------
Write-Host "`n[E] app exit leaves no hotkey residue"
[T069Probe]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
# Poll for the process to be GONE (Get-Process can briefly list an exiting
# process, and Wait-Process throws the moment the name disappears mid-call).
$exited = $false
$deadline = [Environment]::TickCount + 20000
while ([Environment]::TickCount -lt $deadline) {
    $procs = @(Get-Process -Name DockForge -ErrorAction SilentlyContinue |
        Where-Object { -not $_.HasExited })
    if ($procs.Count -eq 0) { $exited = $true; break }
    Start-Sleep -Milliseconds 500
}
if ($exited) {
    Add-Report 'E ok: app exited on WM_CLOSE'
} else {
    Fail 'E: app did not exit after WM_CLOSE'
}
if ([T069Probe]::RegisterHotKey([IntPtr]::Zero, 9300, $MOD_CTRL_ALT, $VK_D)) {
    Add-Report 'E ok: Ctrl+Alt+D registerable after exit (unregistered on the way out)'
    [T069Probe]::UnregisterHotKey([IntPtr]::Zero, 9300) | Out-Null
} else {
    Add-Fail 'E: Ctrl+Alt+D still held after the app exited - residue!'
}

# ---- F: hotspot corner ------------------------------------------------------
Write-Host "`n[F] 12px top-left corner toggles the dock (GetCursorPos, 10Hz)"
$cfg = @{
    hotkeys = @(
        @{ id = 'toggle_dock'; modifiers = 3; vk = 68; action = 'toggle_dock'; enabled = $true }
    )
    hotspots = @(
        @{ id = 'probe.corner'; edge = 'top'; corner = 'tl'; size_px = 12;
           action = 'toggle_dock'; enabled = $true }
    )
}
Write-ConfigJson ($cfg | ConvertTo-Json -Depth 8)
Start-Process -FilePath (Join-Path $root $Exe) -WorkingDirectory $root
Start-Sleep -Seconds 3
$log = Latest-Log
$len = (Get-Item $log.FullName).Length
$dock = [T069Probe]::Find('DockForgeHostWindow', $true)
if ($dock -eq [IntPtr]::Zero) { Fail 'F: dock window not found after relaunch' }
$primary = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$cornerX = $primary.Left + 3; $cornerY = $primary.Top + 3
$centreX = [int](($primary.Left + $primary.Right) / 2)
$centreY = [int](($primary.Top + $primary.Bottom) / 2)
# Park the cursor in the centre first so the corner is a genuine ENTRY.
[T069Probe]::SetCursorPos($centreX, $centreY) | Out-Null
Start-Sleep -Milliseconds 600
$len = (Get-Item (Latest-Log).FullName).Length
[T069Probe]::SetCursorPos($cornerX, $cornerY) | Out-Null
$line = Wait-Log "hotspot 'probe\.corner' entered -> toggle_dock" $len 6000
if ($line) { Add-Report "F ok: '$line'" }
else { Add-Fail 'F: hotspot entry never logged (cursor in corner 3,3)' }
$line = Wait-Log 'action toggle_dock \(0\) fired' $len 6000
if ($line) { Add-Report 'F ok: hotspot dispatched the action' }
else { Add-Fail 'F: hotspot action never fired' }
Start-Sleep -Milliseconds 600
if (-not [T069Probe]::IsWindowVisible($dock)) {
    Add-Report 'F ok: dock HIDDEN after entering the corner'
} else {
    Add-Fail 'F: dock still visible after entering the corner'
}
# Leave (re-arm) and enter again.
[T069Probe]::SetCursorPos($centreX, $centreY) | Out-Null
Start-Sleep -Milliseconds 1200
$len = (Get-Item (Latest-Log).FullName).Length
[T069Probe]::SetCursorPos($cornerX, $cornerY) | Out-Null
$line = Wait-Log "hotspot 'probe\.corner' entered -> toggle_dock" $len 6000
if ($line) { Add-Report 'F ok: re-entry fired again (fire-once + re-arm works)' }
else { Add-Fail 'F: hotspot did not re-fire after leaving and re-entering' }
Start-Sleep -Milliseconds 600
if ([T069Probe]::IsWindowVisible($dock)) {
    Add-Report 'F ok: dock VISIBLE again after the second corner entry'
} else {
    Add-Fail 'F: dock did not come back after the second corner entry'
}

# ---- F2: whole-edge hotspot (corner=null) ------------------------------------
Write-Host "`n[F2] whole top edge (corner=null) also fires"
$cfg = @{
    hotkeys = @(
        @{ id = 'toggle_dock'; modifiers = 3; vk = 68; action = 'toggle_dock'; enabled = $true }
    )
    hotspots = @(
        @{ id = 'probe.edge'; edge = 'top'; size_px = 12; action = 'toggle_dock'; enabled = $true }
    )
}
Write-ConfigJson ($cfg | ConvertTo-Json -Depth 8)
$null = Wait-Log 'applied 1 hotkey entries, 1 registered, 0 failed' (Get-Item (Latest-Log).FullName).Length 8000
[T069Probe]::SetCursorPos($centreX, $centreY) | Out-Null
Start-Sleep -Milliseconds 600
$len = (Get-Item (Latest-Log).FullName).Length
# Centre-top: inside the edge strip but far from every corner.
[T069Probe]::SetCursorPos($centreX, ($primary.Top + 5)) | Out-Null
$line = Wait-Log "hotspot 'probe\.edge' entered -> toggle_dock" $len 6000
if ($line) { Add-Report 'F2 ok: whole-edge strip fired at centre-top' }
else { Add-Fail 'F2: whole-edge hotspot never fired at centre-top' }
Start-Sleep -Milliseconds 400
if (-not [T069Probe]::IsWindowVisible($dock)) {
    Add-Report 'F2 ok: dock hidden by the edge entry'
} else {
    Add-Fail 'F2: dock still visible after the edge entry'
}
[T069Probe]::SetCursorPos($centreX, $centreY) | Out-Null  # leave for G

# ---- G: CPU with hotspots on vs off -----------------------------------------
Write-Host "`n[G] app CPU with hotspots on vs off (8s samples)"
[T069Probe]::SetCursorPos($centreX, $centreY) | Out-Null  # keep the cursor out of the zone
$p = Get-Process -Name DockForge
$c0 = $p.CPU
Start-Sleep -Seconds 8
$p.Refresh()
$on = $p.CPU - $c0
$cfg = @{
    hotkeys = @(
        @{ id = 'toggle_dock'; modifiers = 3; vk = 68; action = 'toggle_dock'; enabled = $true }
    )
    hotspots = @(
        @{ id = 'probe.corner'; edge = 'top'; corner = 'tl'; size_px = 12;
           action = 'toggle_dock'; enabled = $false }
    )
}
Write-ConfigJson ($cfg | ConvertTo-Json -Depth 8)
# Hotspots off -> one more apply whose summary still counts the hotkey.
$null = Wait-Log 'applied 1 hotkey entries, 1 registered, 0 failed' (Get-Item (Latest-Log).FullName).Length 8000
Start-Sleep -Seconds 2
$p = Get-Process -Name DockForge
$c0 = $p.CPU
Start-Sleep -Seconds 8
$p.Refresh()
$off = $p.CPU - $c0
Add-Report ("G: CPU over 8s with hotspots ON = {0:F3}s, OFF = {1:F3}s (delta {2:F3}s, budget one core = 8s)" -f $on, $off, ($on - $off))
if (($on - $off) -lt 0.5) { Add-Report 'G ok: hotspot polling adds < 0.5s CPU per 8s (<6%)' }
else { Add-Fail 'G: hotspot polling cost looks high - check the poll rate' }

# ---- shutdown ----------------------------------------------------------------
$dock = [T069Probe]::Find('DockForgeHostWindow', $false)
if ($dock -ne [IntPtr]::Zero) {
    [T069Probe]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $deadline = [Environment]::TickCount + 15000
    while ([Environment]::TickCount -lt $deadline) {
        $procs = @(Get-Process -Name DockForge -ErrorAction SilentlyContinue |
            Where-Object { -not $_.HasExited })
        if ($procs.Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    }
}
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force

$report | Set-Content -Path (Join-Path $OutDir 'probe.txt') -Encoding UTF8
$fails = @($report | Where-Object { $_ -like 'FAIL:*' }).Count
Write-Host "`n=== probe finished: $($report.Count - $fails) ok, $fails fail ==="
if ($fails -gt 0) { exit 1 } else { exit 0 }
