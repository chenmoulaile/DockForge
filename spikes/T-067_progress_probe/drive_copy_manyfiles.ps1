# T-067 debug: many-small-files copy (per-file overhead stretches the dialog
# lifetime beyond a few polls on fast NVMe, where a single 2GB file finishes
# in ~1.5s and the dialog lives <500ms).
param(
    [int]$Files = 256,
    [int]$FileMB = 4,
    [string]$Dest = 'D:\dockforge-t067-dest'
)
$src = Join-Path $env:TEMP ('dockforge-t067-payload-' + $Files)
if (-not (Test-Path $src)) {
    New-Item -ItemType Directory -Force -Path $src | Out-Null
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    $buf = New-Object byte[] ($FileMB * 1MB)
    for ($i = 0; $i -lt $Files; ++$i) {
        $rng.GetBytes($buf)
        [System.IO.File]::WriteAllBytes((Join-Path $src ("f{0:d4}.bin" -f $i)), $buf)
    }
    $rng.Dispose()
}
Write-Host "payload ready: $Files files x ${FileMB}MB at $src"
Remove-Item -Path (Join-Path $Dest '*') -Recurse -ErrorAction SilentlyContinue
$shell = New-Object -ComObject Shell.Application
$destFolder = $shell.NameSpace($Dest)
if ($null -eq $destFolder) { Write-Host "FAIL NameSpace($Dest)"; exit 1 }
Write-Host 'copy-started (folder)'
$destFolder.CopyHere($src)
Write-Host 'copy-returned'
