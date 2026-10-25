# T-067 debug: dump AppsFolder entry forms.
$shell = New-Object -ComObject Shell.Application
$apps = $shell.NameSpace('shell:AppsFolder')
$n = 0
foreach ($i in $apps.Items()) {
    $n++
    if ($n -le 15 -or $i.Name -match '资源|Explorer|explorer') {
        Write-Host ("name='{0}' path='{1}' type='{2}'" -f $i.Name, $i.Path, $i.Type)
    }
}
Write-Host "total: $n"
