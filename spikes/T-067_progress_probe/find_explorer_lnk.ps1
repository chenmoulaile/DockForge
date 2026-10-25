# T-067 debug: find the Start-Menu .lnk whose pin substring 'explorer' matched.
$dirs = @(
    (Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs'),
    (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs')
)
Get-ChildItem $dirs -Recurse -Filter *.lnk -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match 'explor|资源|管理' } |
    ForEach-Object {
        $sh = New-Object -ComObject WScript.Shell
        $lnk = $sh.CreateShortcut($_.FullName)
        Write-Host ("lnk='{0}' target='{1}'" -f $_.FullName, $lnk.TargetPath)
    }
# And the system accessory shortcuts often carry explorer as target.
Get-ChildItem $dirs -Recurse -Filter *.lnk -ErrorAction SilentlyContinue |
    ForEach-Object {
        $sh = New-Object -ComObject WScript.Shell
        $lnk = $sh.CreateShortcut($_.FullName)
        if ($lnk.TargetPath -match 'explorer\.exe$') {
            Write-Host ("lnk='{0}' target='{1}'" -f $_.FullName, $lnk.TargetPath)
        }
    }
