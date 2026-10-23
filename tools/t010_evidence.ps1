# Capture the full T-010 evidence set for one DPI.
#
# One script per DPI because each phase needs the app restarted with a different
# override, and mixing them in one process would make the DPI ambiguous.
param(
    [Parameter(Mandatory = $true)][string]$OutDir,
    [Parameter(Mandatory = $true)][int]$Dpi,
    [string]$ProcessName = 'DockForge'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$exe = Join-Path $repo 'build\bin\DockForge.exe'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$env:DOCKFORGE_LOG_LEVEL = 'debug'
$env:DOCKFORGE_DPI_OVERRIDE = "$Dpi"
$env:DOCKFORGE_FORCE_MATERIAL = 'solid'
Start-Process -FilePath $exe | Out-Null
Start-Sleep -Seconds 2

$tag = "dpi$Dpi"
$report = Join-Path $OutDir "$tag.txt"
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("dpi_override=$Dpi")

# Park the pointer far from the dock before the idle probes. Otherwise the app is
# still in its hovered state from a previous phase and the "idle" region is really
# the hovered one - which made a correct region look wrong.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class T010Park {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
}
"@ -ErrorAction SilentlyContinue
[T010Park]::SetCursorPos(20, 20) | Out-Null
Start-Sleep -Milliseconds 500

# Region while idle.
& (Join-Path $repo 'tools\region_probe.ps1') -Out (Join-Path $OutDir "$tag.region_idle.txt") | Out-Null
$lines.Add("--- region idle ---")
$lines.AddRange([string[]](Get-Content (Join-Path $OutDir "$tag.region_idle.txt")))

# Screenshot at rest.
& (Join-Path $repo 'tools\screenshot.ps1') -Out (Join-Path $OutDir "$tag.idle.png") | Out-Null

# Measure magnification at an icon centre, which also captures a hovered shot.
& (Join-Path $repo 'tools\hover_measure.ps1') -Out (Join-Path $OutDir "$tag.measure.txt") -IconIndex 5 | Out-Null
$lines.Add("--- magnification at icon 5 ---")
$lines.AddRange([string[]](Get-Content (Join-Path $OutDir "$tag.measure.txt")))

# Region while hovered, taken with the pointer still parked on the icon.
& (Join-Path $repo 'tools\region_probe.ps1') -Out (Join-Path $OutDir "$tag.region_hover.txt") -Hovered | Out-Null
$lines.Add("--- region hovered ---")
$lines.AddRange([string[]](Get-Content (Join-Path $OutDir "$tag.region_hover.txt")))

# Diff the idle and hovered frames: a hover that changes nothing is the failure
# mode this whole task exists to rule out.
& (Join-Path $repo 'tools\image_diff.ps1') `
    -A (Join-Path $OutDir "$tag.measure.txt.idle.png") `
    -B (Join-Path $OutDir "$tag.measure.txt.hover.png") `
    -Out (Join-Path $OutDir "$tag.diff.txt") | Out-Null
$lines.Add("--- idle vs hover diff ---")
$lines.AddRange([string[]](Get-Content (Join-Path $OutDir "$tag.diff.txt")))

Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item Env:\DOCKFORGE_DPI_OVERRIDE -ErrorAction SilentlyContinue

$lines | ForEach-Object { $_ }
[System.IO.File]::WriteAllText($report, ($lines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
