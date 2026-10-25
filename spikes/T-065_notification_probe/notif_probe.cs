// DockForge T-065 spike: UserNotificationListener visibility probe (DoD #1).
//
// Compiled by the machine's stock .NET Framework csc.exe with winmd references
// (PS 5.1's Add-Type could not pass them; see probe log). No SDK "Windows"
// merged winmd exists on this machine, so System.Runtime.WindowsRuntime's AsTask
// is NOT referenced; async operations are drained by polling IAsyncInfo.Status
// directly (IAsyncOperation<T> : IAsyncInfo, projection kept by csc).
//
//   A. activation outside MSIX
//   B. GetAccessStatus() - the SYNCHRONOUS access query (no async needed)
//   C. RequestAccessAsync with a 12s unattended wait (consent dialog state)
//   D. GetNotificationsAsync(Toast) aggregated by AppId -> WeChat/QQ visible?
//   E. exact interface method declaration order (= ABI vtable order) for
//      IUserNotificationListener / IUserNotification, dumped for spike.cpp
using System;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using Windows.Foundation;
using Windows.UI.Notifications;
using Windows.UI.Notifications.Management;

internal static class NotifProbe
{
    private static void DumpInterface(StringBuilder sb, Type iface)
    {
        if (iface == null) { sb.AppendLine("<interface not found>"); return; }
        sb.AppendLine("[" + iface.FullName + "]");
        foreach (var m in iface.GetMethods())
        {
            var ps = new List<string>();
            foreach (var p in m.GetParameters())
            {
                ps.Add(p.ParameterType.Name + " " + p.Name);
            }
            sb.AppendLine("   " + m.ReturnType.Name + " " + m.Name + "(" + string.Join(", ", ps) + ")");
        }
    }

    private static AsyncStatus WaitOp(IAsyncInfo op, int ms)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(ms);
        while (op.Status == AsyncStatus.Started)
        {
            if (DateTime.UtcNow > deadline) break;
            System.Threading.Thread.Sleep(100);
        }
        return op.Status;
    }

    private static string AppIdOf(UserNotification n, StringBuilder sb)
    {
        // Windows.UI.winmd on this OS build no longer projects UserNotification.AppId
        // (superseded by AppInfo). The projected type surface differs from the docs,
        // so probe it by reflection and dump what actually exists once.
        try
        {
            var pi = typeof(UserNotification).GetProperty("AppId");
            if (pi != null)
            {
                var v = (string)pi.GetValue(n);
                if (sb.Length >= 0 && pi.GetValue(n) != null) return v;
            }
        }
        catch { }
        if (sb != null && !appIdsurfaced)
        {
            appIdsurfaced = true;
            sb.Append("   [UserNotification projected properties:");
            foreach (var p in typeof(UserNotification).GetProperties()) sb.Append(" " + p.Name);
            sb.AppendLine("]");
        }
        try
        {
            var ai = typeof(UserNotification).GetProperty("AppInfo");
            if (ai != null)
            {
                var info = ai.GetValue(n);
                var aumi = info.GetType().GetProperty("AppUserModelId") ?? info.GetType().GetProperty("AppUserModelID");
                if (aumi != null) return (string)aumi.GetValue(info);
                sb.Append("   [AppInfo properties:");
                foreach (var p in info.GetType().GetProperties()) sb.Append(" " + p.Name);
                sb.AppendLine("]");
            }
        }
        catch { }
        return "<unreadable>";
    }
    private static bool appIdsurfaced = false;

    private static int Main()
    {
        var sb = new StringBuilder();
        sb.AppendLine("=== T-065 UserNotificationListener probe (csc/.NET projection) ===");
        sb.AppendLine("CLR: " + Environment.Version + "  OS: " + Environment.OSVersion.VersionString);

        UserNotificationListener listener;
        try
        {
            listener = UserNotificationListener.Current;
            sb.AppendLine("A. activate factory   : OK (Current non-null = " + (listener != null) + ")");
        }
        catch (Exception ex)
        {
            sb.AppendLine("A. activate factory   : FAILED 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
            Console.Out.WriteLine(sb.ToString());
            return 1;
        }

        // B. synchronous access status - no consent dialog involved
        UserNotificationListenerAccessStatus status = listener.GetAccessStatus();
        sb.AppendLine("B. GetAccessStatus()  : " + status);

        // E. ABI order
        sb.AppendLine("--- E. projected interface method order (== ABI declaration order) ---");
        foreach (var iface in listener.GetType().GetInterfaces())
        {
            if (iface.Name == "IUserNotificationListener" || iface.Name == "IUserNotificationListenerStatics")
            {
                DumpInterface(sb, iface);
            }
        }

        // C. RequestAccessAsync if not already allowed
        if (status != UserNotificationListenerAccessStatus.Allowed)
        {
            sb.AppendLine("--- C. RequestAccessAsync (12s unattended wait) ---");
            try
            {
                var op = listener.RequestAccessAsync();
                var st = WaitOp(op, 12000);
                if (st == AsyncStatus.Completed)
                {
                    sb.AppendLine("   request completed, access = " + op.GetResults());
                }
                else if (st == AsyncStatus.Error)
                {
                    sb.AppendLine("   request ERROR 0x" + op.ErrorCode.HResult.ToString("X8"));
                }
                else
                {
                    sb.AppendLine("   request status = " + st + " (12s timeout while Started = consent dialog unanswered?)");
                    try { op.Cancel(); } catch { }
                }
            }
            catch (Exception ex)
            {
                sb.AppendLine("   RequestAccessAsync THREW 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
            }
            status = listener.GetAccessStatus();
            sb.AppendLine("   GetAccessStatus() now: " + status);
        }

        // D. visibility
        sb.AppendLine("--- D. GetNotificationsAsync(Toast) ---");
        if (status == UserNotificationListenerAccessStatus.Allowed)
        {
            try
            {
                var op = listener.GetNotificationsAsync(NotificationKinds.Toast);
                var st = WaitOp(op, 10000);
                if (st == AsyncStatus.Completed)
                {
                    IReadOnlyList<UserNotification> notifs = op.GetResults();
                    sb.AppendLine("   total notifications visible = " + notifs.Count);
                    var byApp = new SortedDictionary<string, int>();
                    foreach (var n in notifs)
                    {
                        string appId = AppIdOf(n, sb);
                        if (!byApp.ContainsKey(appId)) byApp[appId] = 0;
                        byApp[appId]++;
                    }
                    foreach (var kv in byApp)
                    {
                        sb.AppendLine("   AppId=" + kv.Key + " count=" + kv.Value);
                    }
                    bool wechat = false, qq = false;
                    foreach (var k in byApp.Keys)
                    {
                        string lower = k.ToLowerInvariant();
                        if (lower.Contains("weixin") || lower.Contains("wechat")) wechat = true;
                        if (lower.Contains("qq") || lower.Contains("tencent")) qq = true;
                    }
                    sb.AppendLine("   WeChat visible  : " + wechat);
                    sb.AppendLine("   QQ visible      : " + qq);

                    if (notifs.Count > 0)
                    {
                        DumpInterface(sb, notifs[0].GetType().GetInterface("IUserNotification"));
                    }
                }
                else if (st == AsyncStatus.Error)
                {
                    sb.AppendLine("   GetNotificationsAsync ERROR 0x" + op.ErrorCode.HResult.ToString("X8"));
                }
                else
                {
                    sb.AppendLine("   GetNotificationsAsync status = " + st);
                    try { op.Cancel(); } catch { }
                }
            }
            catch (Exception ex)
            {
                sb.AppendLine("   GetNotificationsAsync THREW 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
            }
        }
        else
        {
            sb.AppendLine("   skipped: access status = " + status + " (not Allowed)");
        }

        sb.AppendLine("=== probe end ===");
        Console.Out.WriteLine(sb.ToString());
        return 0;
    }
}
