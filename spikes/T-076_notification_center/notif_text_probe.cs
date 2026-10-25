// DockForge T-076 spike, part 2: is the toast TEXT reachable at all?
//
// Part 1 (notif_content_probe.cs) measured that csc's projection of
// Windows.UI.Notifications.Notification exposes only {Visual, ExpirationTime} -
// there is no .Content member, so the documented
// n.Notification.Content.GetBinding(...).GetTextElements() path does not exist
// here. What remains is n.Notification.Visual (NotificationVisual), which is
// documented to carry Bindings. This probe walks it and prints every string it
// can reach, so the sidecar design rests on a measured surface.
//
// Verdict shapes:
//   Q1  Visual reachable? Bindings enumerable? text elements readable?
//   Q2  Fallback: is there any title/body string anywhere in the object graph?
//   Q3  If text is genuinely unavailable: what CAN a notification row show
//       (app display name + creation time only)?
//
// Build (repo root, Git Bash):
//   C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe -nologo -target:exe \
//     -out:spikes/T-076_notification_center/notif_text_probe.exe \
//     -r:System.Runtime.WindowsRuntime.dll \
//     -r:C:/Windows/Microsoft.NET/Framework64/v4.0.30319/System.Runtime.dll \
//     -r:C:/Windows/System32/WinMetadata/Windows.Foundation.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.UI.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.ApplicationModel.winmd \
//     spikes/T-076_notification_center/notif_text_probe.cs
using System;
using System.Collections;
using System.Collections.Generic;
using System.Reflection;
using System.Text;
using System.Threading;
using Windows.Foundation;
using Windows.UI.Notifications;
using Windows.UI.Notifications.Management;

internal static class NotifTextProbe
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
            var pi = o.GetType().GetProperty(name, BindingFlags.Public | BindingFlags.Instance);
            return pi == null ? null : pi.GetValue(o, null);
        }
        catch (Exception ex)
        {
            Console.WriteLine("   [Prop(" + name + ") threw " + ex.GetType().Name + ": " + ex.Message + "]");
            return null;
        }
    }

    private static void ListMembers(object o, string tag)
    {
        if (o == null) { Console.WriteLine("   " + tag + ": <null>"); return; }
        var t = o.GetType();
        Console.WriteLine("   " + tag + ": type=" + t.FullName);
        var props = new List<string>();
        foreach (var pi in t.GetProperties(BindingFlags.Public | BindingFlags.Instance))
            props.Add(pi.Name + ":" + pi.PropertyType.Name);
        Console.WriteLine("     props  = " + string.Join(", ", props.ToArray()));
        var methods = new List<string>();
        foreach (var mi in t.GetMethods(BindingFlags.Public | BindingFlags.Instance))
            if (!mi.Name.StartsWith("get_") && !mi.Name.StartsWith("put_") &&
                mi.Name != "Equals" && mi.Name != "GetHashCode" && mi.Name != "ToString" &&
                mi.Name != "GetType")
                methods.Add(mi.Name);
        Console.WriteLine("     methods= " + string.Join(", ", methods.ToArray()));
    }

    private static int Main()
    {
        var sb = new StringBuilder();
        Console.WriteLine("=== T-076 toast TEXT probe ===");
        var listener = UserNotificationListener.Current;
        Console.WriteLine("access = " + listener.GetAccessStatus());

        var op = listener.GetNotificationsAsync(NotificationKinds.Toast);
        if (WaitOp(op, 10000) != AsyncStatus.Completed) { Console.WriteLine("enumerate failed"); return 3; }
        var notifs = op.GetResults();
        Console.WriteLine("count = " + notifs.Count);
        if (notifs.Count == 0) { Console.WriteLine("no notifications to inspect"); return 4; }

        var n = notifs[0];
        Console.WriteLine("--- id=" + n.Id + " ---");
        ListMembers(n, "UserNotification");
        var toast = Prop(n, "Notification");
        ListMembers(toast, "Notification");
        var visual = Prop(toast, "Visual");
        ListMembers(visual, "Notification.Visual");

        var bindings = Prop(visual, "Bindings");
        ListMembers(bindings, "Notification.Visual.Bindings");
        if (bindings is IEnumerable)
        {
            int i = 0;
            foreach (var b in (IEnumerable)bindings)
            {
                ListMembers(b, "binding[" + i + "]");
                // Text elements: the documented payload carrier.
                var texts = Prop(b, "GetTextElements") ?? Invoke(b, "GetTextElements");
                if (texts is IEnumerable)
                {
                    int k = 0;
                    foreach (var t in (IEnumerable)texts)
                    {
                        Console.WriteLine("     text[" + k + "] = '" + Prop(t, "Text") + "'");
                        ++k;
                    }
                    Console.WriteLine("     -> " + k + " text elements");
                }
                else
                {
                    Console.WriteLine("     GetTextElements -> " + (texts == null ? "<null>" : texts.GetType().Name));
                }
                // Attributes are the other carrier (title uses {{title}} -> value).
                var attrs = Invoke(b, "GetXml") ?? Prop(b, "Xml");
                Console.WriteLine("     xml node = " + (attrs == null ? "<null>" : attrs.GetType().FullName));
                ++i;
                if (i >= 4) break;
            }
            Console.WriteLine("   bindings seen = " + i);
        }

        Console.WriteLine("--- deep string harvest (depth 8) ---");
        var found = new List<string>();
        Harvest(n, "n", 0, found, 8);
        foreach (var f in found) Console.WriteLine("   " + f);
        Console.WriteLine("--- harvest end: " + found.Count + " strings ---");
        return 0;
    }

    private static object Invoke(object o, string method)
    {
        if (o == null) return null;
        try
        {
            var mi = o.GetType().GetMethod(method, BindingFlags.Public | BindingFlags.Instance);
            return mi == null ? null : mi.Invoke(o, null);
        }
        catch (Exception ex)
        {
            Console.WriteLine("   [Invoke(" + method + ") threw " + ex.GetType().Name + ": " + ex.Message + "]");
            return null;
        }
    }

    private static void Harvest(object o, string path, int depth, List<string> found, int max)
    {
        if (o == null || depth > max) return;
        var t = o.GetType();
        if (o is string)
        {
            var s = ((string)o).Trim();
            if (s.Length > 0) found.Add(path + " = " + s);
            return;
        }
        if (t.IsPrimitive || o is DateTime || o is TimeSpan || t.IsEnum) return;
        if (o is IEnumerable && !(o is string))
        {
            int i = 0;
            foreach (var item in (IEnumerable)o)
            {
                Harvest(item, path + "[" + i + "]", depth + 1, found, max);
                if (++i >= 8) break;
            }
            return;
        }
        foreach (var pi in t.GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            if (pi.GetIndexParameters().Length != 0) continue;
            object v;
            try { v = pi.GetValue(o, null); } catch { continue; }
            Harvest(v, path + "." + pi.Name, depth + 1, found, max);
        }
        // Parameterless methods whose names suggest payload access.
        foreach (var mi in t.GetMethods(BindingFlags.Public | BindingFlags.Instance))
        {
            if (mi.GetParameters().Length != 0) continue;
            var nm = mi.Name;
            if (!(nm.StartsWith("Get") || nm.StartsWith("get_"))) continue;
            if (nm == "GetType" || nm == "GetHashCode") continue;
            object v;
            try { v = mi.Invoke(o, null); } catch { continue; }
            Harvest(v, path + "." + nm + "()", depth + 1, found, max);
        }
    }
}
