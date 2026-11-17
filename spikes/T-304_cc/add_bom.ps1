# Adds a UTF-8 BOM to a .ps1 so Windows PowerShell 5.1 reads the Chinese
# strings correctly (ANSI codepage 936 otherwise mangles them).
param([string]$Path)
$c = Get-Content -Raw -Encoding UTF8 $Path
[System.IO.File]::WriteAllText($Path, $c, (New-Object System.Text.UTF8Encoding($true)))
Write-Output ("BOM added: " + $Path)
