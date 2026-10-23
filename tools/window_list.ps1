# tools/window_list.ps1 - list visible top-level windows with their real rects.
#
# Every window-rect measurement in this project goes through a DPI-aware process,
# because an unaware process reads virtualized coordinates and reports sizes that
# do not match what the compositor actually drew (a 2880x1920 screen reads as
# 1440x960). This script sets per-monitor-v2 awareness first, then enumerates.
#
#   powershell.exe -File tools/window_list.ps1
#   powershell.exe -File tools/window_list.ps1 -MinWidth 400 -Filter checker
param(
    [int]$MinWidth = 200,
    [int]$MinHeight = 60,
    [string]$Filter = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class WinList {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }

    public static List<string> List(int minW, int minH) {
        var outp = new List<string>();
        EnumWindows((h, p) => {
            if (!IsWindowVisible(h)) return true;
            var r = new RECT();
            GetWindowRect(h, out r);
            int w = r.Right - r.Left;
            int hh = r.Bottom - r.Top;
            if (w < minW || hh < minH) return true;
            var cn = new StringBuilder(256); GetClassNameW(h, cn, 256);
            var tt = new StringBuilder(256); GetWindowTextW(h, tt, 256);
            uint pid; GetWindowThreadProcessId(h, out pid);
            outp.Add(string.Format("hwnd={0} pid={1} rect={2},{3} size={4}x{5} class={6} title={7}",
                h, pid, r.Left, r.Top, w, hh, cn, tt));
            return true;
        }, IntPtr.Zero);
        return outp;
    }
}
'@

[WinList]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$rows = [WinList]::List($MinWidth, $MinHeight)
if ($Filter) { $rows = $rows | Where-Object { $_ -match $Filter } }
$rows
