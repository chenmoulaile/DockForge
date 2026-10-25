# T-067 debug: sequence of dock screenshots during a copy, correlated with the
# log, to see the bar advance (or not) frame by frame.
param(
    [string]$OutDir = 'C:\Users\chenk\AppData\Local\Temp\t067-seq'
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Remove-Item (Join-Path $OutDir '*') -ErrorAction SilentlyContinue

# Start the copy in the background.
$copy = Start-Process -FilePath 'powershell' -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
    'D:\MacDock\spikes\T-067_progress_probe\drive_copy_manyfiles.ps1',
    '-Files', '256', '-FileMB', '4'
) -PassThru -WindowStyle Hidden

# Screenshot the dock every ~350ms for 12s.
for ($i = 0; $i -lt 34; ++$i) {
    & D:\MacDock\tools\screenshot.ps1 -Out (Join-Path $OutDir ("seq{0:d2}.png" -f $i)) | Out-Null
    Start-Sleep -Milliseconds 200
}
Write-Host "sequence saved: $OutDir"
