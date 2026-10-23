# Appends a markdown fragment to a target file, UTF-8 no BOM.
# Fragments live in their own .md files because a here-string inside a BOM-less
# .ps1 is decoded as ANSI by PowerShell 5.1 and corrupts non-ASCII text.
param(
    [Parameter(Mandatory = $true)][string]$Fragment,
    [Parameter(Mandatory = $true)][string]$Target,
    [string]$Guard = ''
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$targetPath = (Resolve-Path $Target).Path
$text = [System.IO.File]::ReadAllText($targetPath, [System.Text.Encoding]::UTF8)

if ($Guard -ne '' -and $text.Contains($Guard)) {
    Write-Output "guard '$Guard' already present, nothing to do"
    return
}

$fragment = [System.IO.File]::ReadAllText((Resolve-Path $Fragment).Path, [System.Text.Encoding]::UTF8)
$text = $text.TrimEnd() + "`r`n`r`n" + $fragment.TrimEnd() + "`r`n"
[System.IO.File]::WriteAllText($targetPath, $text, (New-Object System.Text.UTF8Encoding($false)))

$check = [System.IO.File]::ReadAllText($targetPath, [System.Text.Encoding]::UTF8)
$ctrl = @($check.ToCharArray() | Where-Object { [int]$_ -lt 32 -and [int]$_ -ne 10 -and [int]$_ -ne 13 })
Write-Output ("appended to {0}: len={1} ctrl_left={2}" -f $targetPath, $check.Length, $ctrl.Count)
