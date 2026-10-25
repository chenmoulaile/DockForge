# T-067 debug helper: drive a copy the way a USER does (visible console),
# vs the probe's hidden-PowerShell variant, to see whether the dialog's
# visibility depends on the driver's window state.
$shell = New-Object -ComObject Shell.Application
$dest = $shell.NameSpace('C:\Users\chenk\AppData\Local\Temp\dockforge-t067-17139765\dest2')
if ($null -eq $dest) {
    New-Item -ItemType Directory -Force -Path 'C:\Users\chenk\AppData\Local\Temp\dockforge-t067-17139765\dest2' | Out-Null
    $dest = $shell.NameSpace('C:\Users\chenk\AppData\Local\Temp\dockforge-t067-17139765\dest2')
}
Write-Host 'copy-started (visible driver)'
$dest.CopyHere('C:\Users\chenk\AppData\Local\Temp\dockforge-t067-17139765\big4096.bin')
Write-Host 'copy-returned'
