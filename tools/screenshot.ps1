# tools/screenshot.ps1 - capture the DockForge window into evidence.
# Usage:
#   pwsh -File tools/screenshot.ps1 -Out evidence/T-001/win.png
#   pwsh -File tools/screenshot.ps1 -Out shot.png -FullScreen
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$ProcessName = 'DockForge',
    [switch]$FullScreen
)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class Dpi {
    [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == -4
    public static void Enable() { SetProcessDpiAwarenessContext(new IntPtr(-4)); }
}
'@
# Without this, GetWindowRect and CopyFromScreen return DPI-virtualized
# coordinates and the capture is a scaled-down, wrong-region crop.
[Dpi]::Enable()

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
}
'@

$outDir = Split-Path -Parent $Out
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

if ($FullScreen) {
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
} else {
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $proc) {
        # Dock is WS_EX_TOOLWINDOW, so MainWindowHandle stays 0; find it by class.
        $sig = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Finder {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public static IntPtr ByClass(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => {
            uint wp; GetWindowThreadProcessId(h, out wp);
            if (wp != pid) return true;
            var sb = new StringBuilder(256); GetClassName(h, sb, 256);
            if (sb.ToString() == cls) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
        if (-not ('Finder' -as [type])) { Add-Type $sig }
        $p = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $p) { Write-Error "process $ProcessName not running"; exit 1 }
        $hwnd = [Finder]::ByClass([uint32]$p.Id, 'DockForgeHostWindow')
        if ($hwnd -eq [IntPtr]::Zero) { Write-Error "window not found"; exit 1 }
    } else {
        $hwnd = $proc.MainWindowHandle
    }

    $r = New-Object W+RECT
    [void][W]::GetWindowRect($hwnd, [ref]$r)
    $bounds = New-Object System.Drawing.Rectangle($r.Left, $r.Top, ($r.Right - $r.Left), ($r.Bottom - $r.Top))
}

$bmp = New-Object System.Drawing.Bitmap($bounds.Width, $bounds.Height)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($bounds.Left, $bounds.Top, 0, 0, $bounds.Size)
$bmp.Save((Resolve-Path -LiteralPath $outDir).Path + '\' + (Split-Path -Leaf $Out), [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose()
$bmp.Dispose()
Write-Output "saved $Out ($($bounds.Width)x$($bounds.Height))"
