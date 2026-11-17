# T-304 debug v3: all top-level windows of the DockForge process (C#-side filter).
$repo = "D:\MacDock"
$exe = "$repo\build\bin\DockForge.exe"
Get-Process DockForge -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
$scratch = "$repo\evidence\T-304\scratch"
Remove-Item "$scratch\cmd.txt","$scratch\state.txt","$scratch\hw_cmd.txt","$scratch\hw_state.txt" -Force -ErrorAction SilentlyContinue
Set-Content -Path "$scratch\cmd.txt" -Value "" -Encoding ASCII
Set-Content -Path "$scratch\hw_cmd.txt" -Value "" -Encoding ASCII
$env:DOCKFORGE_CONFIG_PATH = "$scratch\config.json"
$env:DOCKFORGE_FINDERBAR = "1"
$env:DOCKFORGE_FINDERBAR_CMD_FILE = "$scratch\cmd.txt"
$env:DOCKFORGE_FINDERBAR_STATE_FILE = "$scratch\state.txt"
$env:DOCKFORGE_HW_PANEL_CMD_FILE = "$scratch\hw_cmd.txt"
$env:DOCKFORGE_HW_PANEL_STATE_FILE = "$scratch\hw_state.txt"
$env:DOCKFORGE_FORCE_MATERIAL = "solid"
$proc = Start-Process -FilePath $exe -PassThru
Start-Sleep -Seconds 6
$token = Get-Random
Set-Content -Path "$scratch\cmd.txt" -Value "cc_open`n#dbg-$token" -Encoding ASCII
Start-Sleep -Seconds 2
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class WinEnum3 {
    private delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr h);
    public static string Dump(uint targetPid) {
        var sbOut = new StringBuilder();
        EnumWindows((h, l) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            if (pid == targetPid) {
                var sb = new StringBuilder(256);
                GetClassName(h, sb, 256);
                RECT r; GetWindowRect(h, out r);
                sbOut.Append("hwnd=").Append(h).Append(" class=").Append(sb.ToString())
                     .Append(" visible=").Append(IsWindowVisible(h))
                     .Append(" rect L=").Append(r.L).Append(" T=").Append(r.T)
                     .Append(" R=").Append(r.R).Append(" B=").Append(r.B).AppendLine();
            }
            return true;
        }, IntPtr.Zero);
        return sbOut.ToString();
    }
}
"@
[WinEnum3]::Dump([uint32]$proc.Id)
Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
