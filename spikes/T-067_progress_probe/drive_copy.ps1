# T-067 spike helper: drive an explorer copy-engine file copy (progress dialog
# belongs to a foreign process, exactly the read scenario the DoD names).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File drive_copy.ps1 <src> <dstDir>
param(
    [Parameter(Mandatory = $true)][string]$Src,
    [Parameter(Mandatory = $true)][string]$DstDir
)

$shell = New-Object -ComObject Shell.Application
$dest = $shell.NameSpace($DstDir)
if ($null -eq $dest) { Write-Host "FAIL NameSpace($DstDir)"; exit 1 }
Write-Host "copy-started $Src -> $DstDir"
$dest.CopyHere($Src)
Write-Host "copy-returned"
