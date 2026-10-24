# GPU / 显示拓扑恢复脚本 —— 需管理员
# 依次尝试：重启核显设备 -> 停用两个虚拟显示适配器 -> 检查结果
$ErrorActionPreference = 'Continue'
function Line($t) { Write-Host "`n=== $t ===" -ForegroundColor Cyan }

Line '0. 当前状态'
Get-PnpDevice -Class Display | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize

Line '1. 重启 Intel Arc（Code 43 软复位）'
$gpu = Get-PnpDevice -Class Display | Where-Object { $_.InstanceId -like 'PCI\VEN_8086*' }
if ($gpu) {
    Disable-PnpDevice -InstanceId $gpu.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    Enable-PnpDevice -InstanceId $gpu.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 8
    Get-PnpDevice -InstanceId $gpu.InstanceId | Select-Object Status, FriendlyName |
        ForEach-Object { Write-Host "  -> $($_.FriendlyName) : $($_.Status)" }
}

Line '2. 停用 MuMu 虚拟显示适配器'
$mumu = Get-PnpDevice -Class Display | Where-Object FriendlyName -match 'MuMu Virtual Display'
if ($mumu) {
    Disable-PnpDevice -InstanceId $mumu.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "  -> 已尝试停用 $($mumu.InstanceId)"
}

Line '3. 停用 GameViewer 虚拟显示适配器'
$uu = Get-PnpDevice -Class Display | Where-Object FriendlyName -match 'GameViewer Virtual Display'
if ($uu) {
    Disable-PnpDevice -InstanceId $uu.InstanceId -Confirm:$false -ErrorAction SilentlyContinue
    Write-Host "  -> 已尝试停用 $($uu.InstanceId)"
}

Line '4. 强制单屏拓扑（仅内建面板）'
Add-Type -TypeDefinition @"
using System;using System.Runtime.InteropServices;
public class DSP {
  [StructLayout(LayoutKind.Sequential)] public struct DISPLAY_DEVICE {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=32)] public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceString;
    public int StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string DeviceKey;
  }
  [DllImport("user32.dll", CharSet=CharSet.Ansi)] public static extern bool EnumDisplayDevices(string dev, uint n, ref DISPLAY_DEVICE dd, uint flags);
}
"@
$dd = New-Object DSP+DISPLAY_DEVICE
$dd.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($dd)
$n = 0
while ([DSP]::EnumDisplayDevices($null, $n, [ref]$dd, 0)) {
    Write-Host ("  adapter[{0}] {1} flags=0x{2:X}" -f $n, $dd.DeviceName, $dd.StateFlags)
    $n++
    $dd.cb = [System.Runtime.InteropServices.Marshal]::SizeOf($dd)
}

Line '5. 结果'
Get-PnpDevice -Class Display | ForEach-Object {
    $p = ($_ | Get-PnpDeviceProperty -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
    [PSCustomObject]@{ Status = $_.Status; Problem = $p; Name = $_.FriendlyName }
} | Format-Table -AutoSize

Get-CimInstance Win32_VideoController | Where-Object Name -match 'Intel' |
    Select-Object Name, Status, CurrentRefreshRate, VideoModeDescription | Format-List

Write-Host "`n完成。若 Intel Arc 仍为 Error/43，需要硬断电冷启（见回复第 3 步）。" -ForegroundColor Yellow
