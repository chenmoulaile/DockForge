// DockForge T-076 spike: can the .NET projection read notification CONTENT?
//
// T-065 (spikes/T-065_notification_probe/notif_probe.cs) proved the .NET
// projection completes GetNotificationsAsync (~1s, 51 notifications) where the
// MinGW direct path never does. T-076's pump spike re-measured that boundary:
// STA + a live message queue + 8s of pumping still leaves the IAsyncOperation in
// AsyncStatus::Started. So the notification centre must ride a .NET sidecar,
// exactly like T-068's hw_sidecar.
//
// MEASURED CONSTRAINT (this spike, first compile): csc against the loose
// WinMetadata .winmd files projects a NARROWER surface than the docs describe -
// NotificationBinding.GetXml, Notification.Content and AppInfo.AppUserModelId
// are all absent, and AppInfo comes from Windows.ApplicationModel.winmd, not
// Windows.UI.winmd. The probe therefore walks the object graph by REFLECTION and
// prints what actually exists, which is the only honest way to learn it.
//
//   Q1  Is there body text? (toast -> binding -> text elements / attributes)
//   Q2  Is an app display name readable, or must the dock map AUMID itself?
//   Q3  Are ids stable across two enumerations? (dedupe of new arrivals)
//   Q4  How long does one enumeration take end-to-end? (the poll budget)
//
// Build (repo root, Git Bash):
//   C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe -nologo -target:exe \
//     -out:spikes/T-076_notification_center/notif_content_probe.exe \
//     -r:System.Runtime.WindowsRuntime.dll \
//     -r:C:/Windows/Microsoft.NET/Framework64/v4.0.30319/System.Runtime.dll \
//     -r:C:/Windows/System32/WinMetadata/Windows.Foundation.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.UI.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.ApplicationModel.winmd \
//     spikes/T-076_notification_center/notif_content_probe.cs
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using System.Threading;
using Windows.Foundation;
using Windows.UI.Notifications;
using Windows.UI.Notifications.Management;

internal static class NotifContentProbe
{
    private static AsyncStatus WaitOp(IAsyncInfo op, int ms)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(ms);
        while (op.Status == AsyncStatus.Started)
        {
            if (DateTime.UtcNow > deadline) break;
            Thread.Sleep(50);
        }
        return op.Status;
    }

    private static object Prop(object o, string name)
    {
        if (o == null) return null;
        try
        {
            var pi = o.GetType().GetProperty(name,
                BindingFlags.Public | BindingFlags.Instance);
            return pi == null ? null : pi.GetValue(o, null);
        }
        catch { return null; }
    }

    private static string DumpProps(object o, string tag, StringBuilder sb)
    {
        if (o == null) { sb.AppendLine("   " + tag + " properties: <null>"); return "<null>"; }
        var names = new List<string>();
        foreach (var pi in o.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance))
            names.Add(pi.Name);
        sb.AppendLine("   " + tag + " type=" + o.GetType().FullName);
        sb.AppendLine("   " + tag + " properties: " + string.Join(", ", names.ToArray()));
        return o.GetType().FullName;
    }

    // Every string reachable from a notification, with a depth cap. This is the
    // blunt but honest instrument: it finds the payload without me guessing the
    // projected member names that csc did not emit.
    private static void Harvest(object o, string path, int depth, List<string> found,
                               StringBuilder sb)
    {
        if (o == null || depth > 4) return;
        var t = o.GetType();
        if (o is string)
        {
            var s = ((string)o).Trim();
            if (s.Length > 0) found.Add(path + "=" + s);
            return;
        }
        if (t.IsPrimitive || o is DateTime || o is TimeSpan || t.IsEnum) return;

        if (o is IEnumerable && !(o is string))
        {
            int i = 0;
            foreach (var item in (IEnumerable)o)
            {
                Harvest(item, path + "[" + i + "]", depth + 1, found, sb);
                if (++i >= 12) break;
            }
            return;
        }
        foreach (var pi in t.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            if (pi.GetIndexParameters().Length != 0) continue;
            object v;
            try { v = pi.GetValue(o, null); } catch { continue; }
            Harvest(v, path + "." + pi.Name, depth + 1, found, sb);
        }
    }

    private static int Main()
    {
        var sb = new StringBuilder();
        sb.AppendLine("=== T-076 notification CONTENT probe (.NET projection, reflection) ===");
        sb.AppendLine("CLR: " + Environment.Version + "  OS: " + Environment.OSVersion.VersionString);

        UserNotificationListener listener;
        try { listener = UserNotificationListener.Current; }
        catch (Exception ex)
        {
            sb.AppendLine("A. activate : FAILED 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
            Console.Out.Write(sb.ToString());
            return 1;
        }
        var access = listener.GetAccessStatus();
        sb.AppendLine("A. GetAccessStatus : " + access);
        if (access != UserNotificationListenerAccessStatus.Allowed)
        {
            sb.AppendLine("   not Allowed -> content probe cannot run");
            Console.Out.Write(sb.ToString());
            return 2;
        }

        var sw = System.Diagnostics.Stopwatch.StartNew();
        var op = listener.GetNotificationsAsync(NotificationKinds.Toast);
        var st = WaitOp(op, 10000);
        sw.Stop();
        sb.AppendLine("Q4 enumeration status=" + st + " elapsed=" + sw.ElapsedMilliseconds + "ms");
        if (st != AsyncStatus.Completed)
        {
            sb.AppendLine("   enumeration did not complete -> stopping");
            Console.Out.Write(sb.ToString());
            return 3;
        }
        var notifs = op.GetResults();
        sb.AppendLine("Q4 count=" + notifs.Count);

        // Surface discovery, printed once - this is the deliverable for the sidecar.
        if (notifs.Count > 0)
        {
            var n0 = notifs[0];
            DumpProps(n0, "UserNotification", sb);
            DumpProps(Prop(n0, "AppInfo"), "AppInfo", sb);
            DumpProps(Prop(Prop(n0, "AppInfo"), "DisplayInfo"), "AppInfo.DisplayInfo", sb);
            var toast = Prop(n0, "Notification");
            DumpProps(toast, "Notification", sb);
            var content = Prop(toast, "Content");
            DumpProps(content, "Notification.Content", sb);
            var binding = Prop(content, "ToastGeneric");
            DumpProps(binding, "Content.ToastGeneric", sb);
            sb.AppendLine("--- full string harvest of notification[0] ---");
            var found = new List<string>();
            Harvest(n0, "n", 0, found, sb);
            foreach (var f in found) sb.AppendLine("   " + f);
            sb.AppendLine("--- end harvest ---");
        }

        // Per-notification summary using only members proven above.
        int shown = 0;
        var firstIds = new List<uint>();
        foreach (var n in notifs)
        {
            firstIds.Add(n.Id);
            if (shown >= 10) continue;
            shown++;
            var aumidObj = Prop(Prop(n, "AppInfo"), "AppUserModelId");
            var dispObj = Prop(Prop(Prop(n, "AppInfo"), "DisplayInfo"), "DisplayName");
            sb.AppendLine("--- id=" + n.Id + " created=" + n.CreationTime.ToString("o") +
                           " aumid=" + (aumidObj == null ? "<none>" : aumidObj.ToString()) +
                           " display=" + (dispObj == null ? "<none>" : dispObj.ToString()));
            var texts = new List<string>();
            Harvest(Prop(Prop(Prop(n, "Notification"), "Content"), "ToastGeneric"),
                    "binding", 0, texts, sb);
            foreach (var tx in texts) sb.AppendLine("     " + tx);
        }

        var op2 = listener.GetNotificationsAsync(NotificationKinds.Toast);
        var st2 = WaitOp(op2, 10000);
        if (st2 == AsyncStatus.Completed)
        {
            var again = op2.GetResults();
            var second = new List<uint>();
            foreach (var n in again) second.Add(n.Id);
            bool same = firstIds.Count == second.Count;
            if (same)
            {
                for (int i = 0; i < firstIds.Count; i++)
                    if (firstIds[i] != second[i]) { same = false; break; }
            }
            sb.AppendLine("Q3 id stability : first=" + firstIds.Count + " second=" + second.Count +
                           " identical=" + same);
        }
        else
        {
            sb.AppendLine("Q3 id stability : second enumeration status=" + st2);
        }

        sb.AppendLine("=== probe end ===");
        Console.Out.Write(sb.ToString());
        return 0;
    }
}
