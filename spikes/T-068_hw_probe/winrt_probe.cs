// DockForge T-068 spike: GSMTC + Radios through the official .NET projection.
//
// Purpose (mirrors the T-065 notif probe):
//   A. Can an unpackaged console process use GSMTC at all (RequestAsync)?
//   B. What is the exact projected interface method declaration order
//      (== ABI vtable order) for the interfaces the MinGW direct spike needs?
//   C. Same two questions for Windows.Devices.Radios (GetRadiosAsync, state).
//
// Control commands (TryPlayAsync etc.) are deliberately NOT sent here: with a
// real media session a pause would hit the user's player. Playback control is
// exercised by the machine probe with a controlled media source instead.
//
// Build (Git Bash, repo root; no Windows SDK on this machine, so the stock
// .NET Framework csc + system winmd files, exactly like T-065):
//   C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe -nologo -target:exe \
//     -out:spikes/T-068_hw_probe/winrt_probe.exe \
//     -r:System.Runtime.WindowsRuntime.dll \
//     -r:C:/Windows/System32/WinMetadata/Windows.Foundation.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.Media.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.Devices.winmd \
//     spikes/T-068_hw_probe/winrt_probe.cs
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using Windows.Foundation;
using Windows.Media.Control;
using Windows.Devices.Radios;

internal static class WinrtProbe
{
    private static bool Drain(IAsyncInfo op, int ms, StringBuilder sb, string tag)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(ms);
        while (op.Status == AsyncStatus.Started)
        {
            if (DateTime.UtcNow > deadline) break;
            System.Threading.Thread.Sleep(100);
        }
        sb.AppendLine("   " + tag + " -> status=" + op.Status +
                      (op.Status == AsyncStatus.Error
                           ? " err=" + op.ErrorCode
                           : ""));
        return op.Status == AsyncStatus.Completed;
    }

    private static void DumpInterface(StringBuilder sb, Type t, string want)
    {
        if (t == null) { sb.AppendLine("[" + want + ": NOT PROJECTED]"); return; }
        sb.AppendLine("[" + t.FullName + "]");
        foreach (var m in t.GetMethods())
        {
            var ps = new List<string>();
            foreach (var p in m.GetParameters()) ps.Add(p.ParameterType.Name + " " + p.Name);
            sb.AppendLine("   " + m.ReturnType.Name + " " + m.Name + "(" + string.Join(", ", ps) + ")");
        }
    }

    private static void DumpOn(StringBuilder sb, object o, string want)
    {
        foreach (var i in o.GetType().GetInterfaces())
            if (i.Name == want) { DumpInterface(sb, i, want); return; }
        sb.AppendLine("[" + want + ": not in " + o.GetType().Name + " interface list]");
    }

    private static int Main()
    {
        var sb = new StringBuilder();
        sb.AppendLine("=== T-068 WinRT probe (csc/.NET projection): GSMTC + Radios ===");
        sb.AppendLine("CLR: " + Environment.Version + "  OS: " + Environment.OSVersion.VersionString);

        // ---- A/B: GSMTC ----------------------------------------------------
        GlobalSystemMediaTransportControlsSessionManager mgr = null;
        try
        {
            var op = GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            if (Drain(op, 15000, sb, "A. RequestAsync"))
            {
                mgr = op.GetResults();
                sb.AppendLine("A. GSMTC manager                 : OK (" + (mgr != null) + ")");
            }
        }
        catch (Exception ex)
        {
            sb.AppendLine("A. RequestAsync threw            : 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
        }

        if (mgr != null)
        {
            var sessions = mgr.GetSessions();
            sb.AppendLine("B. GetSessions().Size            : " + sessions.Count);
            var cur = mgr.GetCurrentSession();
            sb.AppendLine("B. GetCurrentSession             : " +
                          (cur == null ? "<null>" : cur.SourceAppUserModelId));
            if (cur != null)
            {
                var info = cur.GetPlaybackInfo();
                sb.AppendLine("B. playback status               : " +
                              (info == null ? "<null>" : info.PlaybackStatus.ToString()));
                var t = cur.GetTimelineProperties();
                sb.AppendLine("B. timeline                      : pos=" + t.Position +
                              " start=" + t.StartTime + " end=" + t.EndTime);
                var mprop = cur.TryGetMediaPropertiesAsync();
                if (Drain(mprop, 10000, sb, "B. TryGetMediaPropertiesAsync"))
                {
                    var mp = mprop.GetResults();
                    sb.AppendLine("B. media props                   : title='" + mp.Title +
                                  "' artist='" + mp.Artist + "' album='" + mp.AlbumTitle + "'");
                }
            }
        }

        // ---- ABI order dump (vtable source for the MinGW direct spike) -----
        sb.AppendLine("--- E. projected interface method order (== ABI declaration order) ---");
        DumpInterface(sb, typeof(GlobalSystemMediaTransportControlsSessionManager).GetInterface(
            "IGlobalSystemMediaTransportControlsSessionManagerStatics"), "Statics");
        DumpInterface(sb, typeof(GlobalSystemMediaTransportControlsSessionManager).GetInterface(
            "IGlobalSystemMediaTransportControlsSessionManager"), "Manager");
        if (mgr != null) DumpOn(sb, mgr, "IGlobalSystemMediaTransportControlsSessionManager");
        DumpInterface(sb, typeof(GlobalSystemMediaTransportControlsSession).GetInterface(
            "IGlobalSystemMediaTransportControlsSession"), "Session");
        DumpInterface(sb, typeof(GlobalSystemMediaTransportControlsSessionMediaProperties).GetInterface(
            "IGlobalSystemMediaTransportControlsSessionMediaProperties"), "MediaProperties");

        // ---- C: Radios ------------------------------------------------------
        try
        {
            var op = Radio.GetRadiosAsync();
            if (Drain(op, 15000, sb, "C. Radio.GetRadiosAsync"))
            {
                var radios = op.GetResults();
                sb.AppendLine("C. radios visible                : " + radios.Count);
                foreach (var r in radios)
                {
                    sb.AppendLine("   radio kind=" + r.Kind + " name='" + r.Name + "' state=" + r.State);
                }
            }
        }
        catch (Exception ex)
        {
            sb.AppendLine("C. GetRadiosAsync threw          : 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
        }
        sb.AppendLine("--- E2. Radios interface order ---");
        DumpInterface(sb, typeof(Radio).GetInterface("IRadioStatics"), "IRadioStatics");
        DumpInterface(sb, typeof(Radio).GetInterface("IRadio"), "IRadio");

        Console.Out.Write(sb.ToString());
        return 0;
    }
}
