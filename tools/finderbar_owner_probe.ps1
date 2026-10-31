# T-301 owner-drawn menu probe.
#
# Modern menu bars (Sublime Text, VS Code) put MFT_OWNERDRAW items in the top-level menu
# with empty text, so GetMenuStringW returns nothing and the bar used to fall back to the
# application name alone. This probe drives the real path end to end against Sublime Text:
# read the titles, then forward one command and prove the application acted on it.
#
# Why Sublime and not a fixture: the fixture (spikes/T-301_finderbar_menu/menu_target.cpp)
# has ordinary text menus, so it cannot exercise the owner-draw branch at all. Sublime is
# the application that made the gap visible in the first place.
param(
    [string]$OutDir = 'evidence\T-301',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$SublimeExe = 'D:\Sublime Text\sublime_text.exe'
)

$ErrorActionPreference = 'Stop'
$script:failures = 0
function Check([string]$name, [bool]$ok, [string]$detail = '') {
    if ($ok) {
        Write-Host "PASS: $name"
    } else {
        Write-Host "FAIL: $name $detail"
        $script:failures++
    }
}
function Stop-All {
    foreach ($n in @('DockForge', 'sublime_text')) {
        Get-Process $n -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 600
}

$outFull = (Resolve-Path $OutDir).Path
$scratch = Join-Path $env:TEMP ("dockforge-t301-owner-" + [DateTime]::Now.Ticks)
New-Item -ItemType Directory -Path $scratch -Force | Out-Null
$cmdFile = Join-Path $scratch 'cmd.txt'
$stateFile = Join-Path $scratch 'state.json'
$configFile = Join-Path $scratch 'config.json'
[System.IO.File]::WriteAllText($configFile, '{}', (New-Object System.Text.UTF8Encoding($false)))

Stop-All

if (-not (Test-Path $SublimeExe)) {
    Write-Host "SKIP: Sublime Text not installed at $SublimeExe"
    exit 0
}
Start-Process $SublimeExe
Start-Sleep -Seconds 8
$app = Get-Process sublime_text | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
Check 'Sublime Text started with a top-level window' ($null -ne $app)
if ($null -eq $app) { exit 1 }
$hwndHex = '{0:x}' -f $app.MainWindowHandle.ToInt64()

$env:DOCKFORGE_CONFIG_PATH = $configFile
$env:DOCKFORGE_LOG_DIR = $outFull
$env:DOCKFORGE_DUMP_DIR = $outFull
$env:DOCKFORGE_FINDERBAR = '1'
$env:DOCKFORGE_FINDERBAR_CMD_FILE = $cmdFile
$env:DOCKFORGE_FINDERBAR_STATE_FILE = $stateFile
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
Start-Process ((Resolve-Path $Exe).Path)
Start-Sleep -Seconds 4

function Send-Cmd([string]$text, [int]$settleMs = 1200) {
    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($cmdFile, "$text`n#$([Guid]::NewGuid())",
        (New-Object System.Text.UTF8Encoding($false)))
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 100
        if (Test-Path $stateFile) { break }
    }
    Start-Sleep -Milliseconds $settleMs
    return (Get-Content $stateFile -Encoding UTF8 -Raw | ConvertFrom-Json)
}

$snap = Send-Cmd "attach $hwndHex"
$snap = Send-Cmd 'state'

Check 'the bar tracks the pinned application' ($snap.foreground_name -eq 'Sublime Text') "name=$($snap.foreground_name)"
Check 'the owner-drawn menu was read at all' ($snap.menu_readable -eq $true)
$titles = @($snap.menu_titles)
Check 'six top-level titles were recovered from dwItemData' ($titles.Count -eq 6) "titles=$($titles -join ',')"
Check 'the titles are the real menu bar' (($titles -join ',') -eq 'File,Edit,Selection,Find,View,Goto') "titles=$($titles -join ',')"
Check 'no title carries a leftover accelerator marker' (-not ($titles -match '&'))

$menuBoxes = @($snap.boxes | Where-Object { $_.menu_index -ge 0 })
Check 'every recovered title got a box on the bar' ($menuBoxes.Count -eq 6) "boxes=$($menuBoxes.Count)"
Check 'the boxes are ordered left to right' (
    ($menuBoxes | ForEach-Object { [double]$_.left }) -join ',' -eq
    (($menuBoxes | ForEach-Object { [double]$_.left } | Sort-Object) -join ','))

# File > New File carries command id 0, which is exactly the value the old guard rejected
# as "no command". The bar's own INFO line reports whether the post went out, so the probe
# stops there: reading the application's internal state (tab count, document title) would
# couple the test to Sublime's UI and told us nothing about the bar.
$logPath0 = (Get-ChildItem (Join-Path $outFull 'dockforge-*.log') |
             Sort-Object LastWriteTime | Select-Object -Last 1).FullName
$snap = Send-Cmd 'pick 2 0' 1200
Check 'the command was forwarded' ($snap.menu_open -eq $false)
$forwarded = Get-Content $logPath0 -Encoding UTF8 | Select-String -Pattern 'probe pick box=2 item=0 sent=1'
Check 'command id 0 is forwarded rather than discarded' ($null -ne $forwarded)
$posted = Get-Content $logPath0 -Encoding UTF8 | Select-String -Pattern 'forwarded WM_COMMAND 0 to'
Check 'the forwarded message reached the application window' ($null -ne $posted)

# Items that open a submenu or sit between groups have no command id, and forwarding one
# would send a meaningless WM_COMMAND. The refusal is observable in the probe's own INFO
# line (sent=0) rather than in the DEBUG line, because the default log level drops DEBUG -
# measured: items 3 and 4 of Sublime's File menu are the separators.
$logPath = (Get-ChildItem (Join-Path $outFull 'dockforge-*.log') |
            Sort-Object LastWriteTime | Select-Object -Last 1).FullName
$null = Send-Cmd 'pick 2 3' 800
$refused = Get-Content $logPath -Encoding UTF8 | Select-String -Pattern 'probe pick box=2 item=3 sent=0'
Check 'a separator is refused instead of forwarded' ($null -ne $refused)
$null = Send-Cmd 'pick 2 1' 800
$accepted = Get-Content $logPath -Encoding UTF8 | Select-String -Pattern 'probe pick box=2 item=1 sent=1'
Check 'a real command next to it still forwards' ($null -ne $accepted)

Stop-All
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue

if ($script:failures -eq 0) {
    Write-Host 'verdict: Q24_ownerdraw_titles=PASS Q25_ownerdraw_forwarding=PASS'
    exit 0
}
Write-Host "verdict: FAILURES=$($script:failures)"
exit 1