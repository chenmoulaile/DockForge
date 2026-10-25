# DockForge T-060 evidence probe: logon-trigger autostart via Task Scheduler.
#
# Part A - system chain through the production module (COM ITaskService):
#   1) autostart_probe.exe on      -> task \DockForge registered (logon trigger)
#   2) schtasks /query             -> cross-check from the OS side
#   3) schtasks /query /xml        -> export the real definition; copies land in
#                                     evidence/T-060/ and tools/autostart/ (audit)
#   4) autostart_probe.exe off     -> task deleted, no residue
#   5) autostart_probe.exe off     -> idempotent: second off still succeeds
#
# Part B - the settings switch actually working (T-050 probe channel):
#   set behavior.autostart 1 -> flush saves config.json, ConfigChanged publishes,
#                               the subscription registers the real task
#   set behavior.autostart 0 -> the subscription deletes it again
#   (fresh profile default: no task is created on start - DoD "默认关闭")
#
# Log-driven like the other probes: the app's own log lines decide PASS/FAIL,
# schtasks only provides the independent OS-side confirmation.
param(
    [string]$OutDir = 'evidence\T-060',
    [string]$Exe = 'build\bin\DockForge.exe',
    [string]$ProbeExe = 'build\bin\autostart_probe.exe'
)
$ErrorActionPreference = 'Stop'
Get-Process -Name DockForge -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400

# Belt and braces: remove any leftover task from an earlier crashed run so the
# probe starts from a clean slate.
& $ProbeExe off | Out-Null
Start-Sleep -Milliseconds 300

$RepoRoot = (Get-Location).Path
$Evidence = Join-Path $RepoRoot $OutDir
New-Item -ItemType Directory -Force -Path $Evidence | Out-Null
$TempDir = Join-Path $env:TEMP ('dockforge-t060-' + [Environment]::TickCount)
$TempConfig = Join-Path $TempDir 'config.json'
New-Item -ItemType Directory -Force -Path $TempDir | Out-Null
$CmdFile = Join-Path $env:TEMP 'df_settings_probe_cmd.txt'
[System.IO.File]::WriteAllText($CmdFile, '', (New-Object System.Text.UTF8Encoding($false)))

$report = New-Object System.Collections.Generic.List[string]
$failures = 0
function Add-Report([string]$Line) { $script:report.Add($Line); Write-Output $Line }
function Add-Fail([string]$Line) { $script:failures++; Add-Report "FAIL: $Line" }
function Set-Command([string]$Command) {
    [System.IO.File]::WriteAllText($CmdFile, $Command,
        (New-Object System.Text.UTF8Encoding($false)))
}
# schtasks console output is CP936 on this box; decode with the console encoding
# so the exported XML (pure ASCII) is not mangled on the way through.
# NOTE: the parameter must NOT be named $Args - that collides with the automatic
# variable and binds empty (measured: count=0, so every /tn silently became a
# whole-store query, rc=0, and Task-Exists read true forever).
function Capture-Schtasks([string[]]$SchTasksArgs) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = 'schtasks'
    $psi.Arguments = ($SchTasksArgs -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::GetEncoding(936)
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd()
    $err = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    return @{ Out = $out; Err = $err; Code = $p.ExitCode }
}
function Task-Exists {
    (Capture-Schtasks @('/query', '/tn', 'DockForge')).Code -eq 0
}
function Get-LatestLog {
    $dir = Join-Path $env:LOCALAPPDATA 'DockForge\logs'
    Get-ChildItem $dir -Filter 'dockforge-*.log' | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
function Wait-LogMatch([long]$Offset, [string]$Pattern, [int]$TimeoutMs = 8000) {
    $log = Get-LatestLog
    $deadline = [Environment]::TickCount + $TimeoutMs
    while ([Environment]::TickCount -lt $deadline) {
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
        $log = Get-LatestLog
    }
    return $null
}

Add-Report "== T-060 autostart probe $(Get-Date -Format o) =="
Add-Report "exe=$Exe probe=$ProbeExe tempconfig=$TempConfig"

# ---- Part A: production module against the real scheduler -------------------
Add-Report "--- Part A: system chain (autostart_probe.exe) ---"

Add-Report "[A1] default state: task must be absent before anything runs"
if (Task-Exists) { Add-Fail "A1: task \DockForge already exists before the probe" }
else { Add-Report "A1 ok: task absent (schtasks /query rc!=0)" }

Add-Report "[A2] autostart_probe.exe on -> register logon-trigger task"
$on = & $ProbeExe on 2>&1
Add-Report "A2 output: $on"
if ($LASTEXITCODE -ne 0) { Add-Fail "A2: 'on' returned rc=$LASTEXITCODE" }

Add-Report "[A3] schtasks /query cross-check (OS side)"
$q = Capture-Schtasks @('/query', '/tn', 'DockForge', '/v', '/fo', 'LIST')
Add-Report ($q.Out.Trim() -split "`r?`n" | Where-Object { $_ -match '任务名|TaskName|Status|状态|下次|Next|运行|Run|作者|Author' } |
    ForEach-Object { $_.Trim() } | Select-Object -First 6 | Out-String).Trim()
if ($q.Code -ne 0) { Add-Fail "A3: schtasks /query rc=$($q.Code): $($q.Err.Trim())" }

Add-Report "[A4] export the real task definition XML (audit)"
$x = Capture-Schtasks @('/query', '/tn', 'DockForge', '/xml')
if ($x.Code -ne 0) {
    Add-Fail "A4: xml export rc=$($x.Code): $($x.Err.Trim())"
} else {
    $xmlText = $x.Out
    $evidenceXml = Join-Path $Evidence 'autostart-task.xml'
    $auditXml = Join-Path $RepoRoot 'tools\autostart\DockForge.xml'
    [System.IO.File]::WriteAllText($evidenceXml, $xmlText, (New-Object System.Text.UTF8Encoding($false)))
    [System.IO.File]::WriteAllText($auditXml, $xmlText, (New-Object System.Text.UTF8Encoding($false)))
    Add-Report "A4 exports: $evidenceXml ; $auditXml ($($xmlText.Length) chars)"
    # ApplyAutostartSystem registers the CALLING module's own path
    # (GetModuleFileNameW(nullptr)); in Part A the caller is the probe exe, so
    # the command must be the probe, not DockForge. The product-path check
    # (command = DockForge.exe) runs in Part B where DockForge.exe itself
    # registers through the settings switch.
    $exeFull = (Get-Item (Join-Path $RepoRoot $ProbeExe)).FullName
    $checks = @(
        @{ Name = 'LogonTrigger present';   Ok = $xmlText.Contains('<LogonTrigger') },
        @{ Name = 'InteractiveToken logon'; Ok = $xmlText.Contains('InteractiveToken') },
        @{ Name = 'command = caller exe';   Ok = $xmlText.Contains($exeFull) },
        @{ Name = 'not a registry Run key'; Ok = $xmlText.Contains('schemas.microsoft.com/windows/2004/02/mit/task') }
    )
    foreach ($c in $checks) {
        if ($c.Ok) { Add-Report "A4 ok: $($c.Name)" } else { Add-Fail "A4: $($c.Name) not satisfied" }
    }
}

Add-Report "[A5] autostart_probe.exe off -> delete the task"
$off = & $ProbeExe off 2>&1
Add-Report "A5 output: $off"
if ($LASTEXITCODE -ne 0) { Add-Fail "A5: 'off' returned rc=$LASTEXITCODE" }
if (Task-Exists) { Add-Fail "A5: task still present after off" }
else { Add-Report "A5 ok: task gone (schtasks /query rc!=0)" }

Add-Report "[A6] idempotent second off (nothing to delete)"
$off2 = & $ProbeExe off 2>&1
Add-Report "A6 output: $off2"
if ($LASTEXITCODE -ne 0) { Add-Fail "A6: second 'off' returned rc=$LASTEXITCODE" }
else { Add-Report "A6 ok: off is idempotent" }

# ---- Part B: the settings switch, production chain --------------------------
Add-Report "--- Part B: settings switch -> real task (probe channel) ---"

$offset = (Get-Item (Get-LatestLog).FullName).Length
$env:DOCKFORGE_SETTINGS = '1'
$env:DOCKFORGE_SETTINGS_CMD_FILE = $CmdFile
$env:DOCKFORGE_CONFIG_PATH = $TempConfig
$env:DOCKFORGE_LOG_LEVEL = 'debug'
$proc = Start-Process -FilePath (Join-Path $RepoRoot $Exe) -PassThru
Start-Sleep -Milliseconds 3000

Add-Report "[B1] fresh profile, default config: no task may appear (默认关闭)"
if (Task-Exists) { Add-Fail "B1: a default-off profile created a task" }
else { Add-Report "B1 ok: no task after startup with behavior.autostart=false" }

Add-Report "[B2] set behavior.autostart 1 (RecordEdit -> debounce -> SaveConfig -> publish)"
Set-Command 'set behavior.autostart 1'
$line = Wait-LogMatch $offset 'autostart switch -> 1'
if ($line) { Add-Report "B2 log: $($line.Trim())" }
else { Add-Fail "B2: no 'autostart switch -> 1' log line" }
Start-Sleep -Milliseconds 800
if (Task-Exists) { Add-Report "B2 ok: task \DockForge registered by the switch" }
else { Add-Fail "B2: switch on did not create the task" }

Add-Report "[B2b] export while the app's own task is live: command must be DockForge.exe"
$x2 = Capture-Schtasks @('/query', '/tn', 'DockForge', '/xml')
if ($x2.Code -ne 0) {
    Add-Fail "B2b: xml export rc=$($x2.Code): $($x2.Err.Trim())"
} else {
    $settingsXml = Join-Path $Evidence 'autostart-task-settings.xml'
    [System.IO.File]::WriteAllText($settingsXml, $x2.Out, (New-Object System.Text.UTF8Encoding($false)))
    # The repo audit copy under tools/autostart/ gets the app-registered
    # definition (Part A's copy shows the caller-exe variant).
    $auditXml2 = Join-Path $RepoRoot 'tools\autostart\DockForge.xml'
    [System.IO.File]::WriteAllText($auditXml2, $x2.Out, (New-Object System.Text.UTF8Encoding($false)))
    $appFull = (Get-Item (Join-Path $RepoRoot $Exe)).FullName
    $checks2 = @(
        @{ Name = 'command = DockForge.exe';  Ok = $x2.Out.Contains($appFull) },
        @{ Name = 'LogonTrigger present';     Ok = $x2.Out.Contains('<LogonTrigger') },
        @{ Name = 'InteractiveToken logon';   Ok = $x2.Out.Contains('InteractiveToken') }
    )
    foreach ($c in $checks2) {
        if ($c.Ok) { Add-Report "B2b ok: $($c.Name)" } else { Add-Fail "B2b: $($c.Name) not satisfied" }
    }
    Add-Report "B2b exports: $settingsXml ; $auditXml2 ($($x2.Out.Length) chars)"
}

Add-Report "[B3] the switch wrote the flag to disk (config.json)"
$configText = if (Test-Path $TempConfig) { [System.IO.File]::ReadAllText($TempConfig) } else { '' }
$autostartOn = $configText -match '"autostart"\s*:\s*true'
Add-Report "B3 config.json match autostart:true = $autostartOn ($(if ($configText) { $configText.Length } else { 0 }) bytes)"
if (-not $autostartOn) { Add-Fail "B3: behavior.autostart=true not found in config.json" }

Add-Report "[B4] set behavior.autostart 0 -> delete, no residue"
Set-Command 'set behavior.autostart 0'
$line0 = Wait-LogMatch $offset 'autostart switch -> 0'
if ($line0) { Add-Report "B4 log: $($line0.Trim())" }
else { Add-Fail "B4: no 'autostart switch -> 0' log line" }
Start-Sleep -Milliseconds 800
if (Task-Exists) { Add-Fail "B4: task still present after switch off" }
else { Add-Report "B4 ok: task deleted by the switch (no residue)" }

Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500

# ---- cleanup + verdict ------------------------------------------------------
Add-Report "--- cleanup ---"
& $ProbeExe off | Out-Null
if (Task-Exists) { Add-Fail "cleanup: task still present at the end" }
else { Add-Report "cleanup ok: no scheduled task left" }
Remove-Item -Recurse -Force $TempDir -ErrorAction SilentlyContinue
Remove-Item -Force $CmdFile -ErrorAction SilentlyContinue

$verdict = if ($failures -eq 0) { 'PASS' } else { "FAIL ($failures)" }
Add-Report "== verdict=$verdict =="
[System.IO.File]::WriteAllLines((Join-Path $Evidence 'verify.txt'), $report,
    (New-Object System.Text.UTF8Encoding($false)))
exit $(if ($failures -eq 0) { 0 } else { 1 })
