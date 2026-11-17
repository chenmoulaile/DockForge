# T-304 spike appendix: does this machine expose brightness via WMI (root/wmi)?
# The dxva2 DDC/CI path failed with ERROR_NOT_CONNECTED (1168) on the internal
# panel; WmiMonitorBrightness / WmiMonitorBrightnessMethods is the other
# documented surface (laptop eDP panels). Read-only probe.
try {
    $b = Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightness -ErrorAction Stop
    Write-Output ("WmiMonitorBrightness: CurrentBrightness=" + $b.CurrentBrightness + " Levels=" + $b.Levels)
    $m = Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightnessMethods -ErrorAction Stop
    Write-Output ("WmiMonitorBrightnessMethods: present instance=" + ($m -ne $null))
} catch {
    Write-Output ("WMI brightness probe FAILED: " + $_.Exception.Message)
}
