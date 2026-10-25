// DockForge T-076 notification sidecar.
//
// WHY THIS EXISTS (measured, not assumed):
//   - The MinGW main process cannot drive Windows.UI.Notifications.Management.
//     UserNotificationListener: every entry point is an IAsyncOperation, and in
//     this process those never complete. spikes/T-076_notification_center/spike.cpp
//     built the most favourable case available (RoInitialize STA + a real
//     message queue + 8s of PeekMessage/DispatchMessage) and GetNotificationsAsync
//     still sat at AsyncStatus.Started forever - see
//     evidence/T-076/spike_async_pump.log. So the boundary is the WinRT/C++ ABI
//     in a MinGW process, not the apartment or the pump.
//   - The .NET projection completes the same calls in 183ms with 62
//     notifications and stable ids across enumerations
//     (evidence/T-076/spike_notif_content.log). Only the stock .NET Framework
//     that every Windows machine ships is needed - no SDK, no C++/WinRT.
//   - The toast text is NOT reachable through Notification.Content (that member
//     does not exist in this projection). It is reachable through
//     Notification.Visual.Bindings[i].GetTextElements()[k].Text
//     (evidence/T-076/spike_notif_text.log).
//
// PROTOCOL (one JSON object per line, UTF-8 without BOM), the same shape as
// tools/hw_sidecar.cs so the C++ side can reuse one transport:
//   -> poll
//   <- {"ok":true,"access":1,"count":62,"items":[
//         {"id":13825,"aumid":"com.qoder.app","app":"Qoder",
//          "title":"...","body":"...","created":1690000000}]}
//      access: UserNotificationListenerAccessStatus
//              0=Unspecified 1=Allowed 2=Denied
//      created: Unix seconds (UTC). id is the listener's UInt32, stable across
//      enumerations, so the main process dedups on it.
//   -> clear
//   <- {"ok":true}                      (UserNotificationListener.ClearNotifications)
//   -> remove <id>
//   <- {"ok":true} or {"ok":false,"error":"..."}
//   -> quit
// Push lines: {"push":"notifications"} when the listener's NotificationChanged
// event fires, throttled to >=250ms apart. The main process answers with poll.
//
// PRIVACY (ADR-0025): nothing is written to disk anywhere in this file. The
// notification text exists only in the process's own memory and in the pipe to
// the dock; no log file, no cache, no telemetry.
//
// Build (repo root, Git Bash; the stock .NET Framework csc, no SDK):
//   C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe -nologo -target:exe \
//     -out:build/bin/notification_sidecar.exe \
//     -r:System.Runtime.WindowsRuntime.dll \
//     -r:C:/Windows/Microsoft.NET/Framework64/v4.0.30319/System.Runtime.dll \
//     -r:C:/Windows/System32/WinMetadata/Windows.Foundation.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.UI.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.ApplicationModel.winmd \
//     tools/notification_sidecar.cs
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Text;
using System.Threading;
using Windows.Foundation;
using Windows.UI.Notifications;
using Windows.UI.Notifications.Management;

internal static class NotificationSidecar
{
    private const int kMaxItems = 40;         // history panel depth
    private const int kMaxTitleChars = 200;   // a runaway toast must not blow up a row
    private const int kMaxBodyChars = 600;
    private static readonly object gate = new object();
    private static DateTime lastPush = DateTime.MinValue;
    private static int mainPid;

    // Hand-rolled JSON string escaping: the same trade as hw_sidecar.cs (no
    // dependency on a serializer that may not be on a bare .NET Framework).
    private static string J(string s)
    {
        if (s == null) return "\"\"";
        var sb = new StringBuilder(s.Length + 8);
        sb.Append('"');
        foreach (var c in s)
        {
            switch (c)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                case '\n': sb.Append("\\n"); break;
                case '\r': sb.Append("\\r"); break;
                case '\t': sb.Append("\\t"); break;
                default:
                    if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                    else sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
        return sb.ToString();
    }

    // Polls an IAsyncOperation instead of awaiting it: this file targets the
    // bare .NET Framework where the async/await WinRT interop helpers are not
    // guaranteed to be referenceable, and the spike proved Status polling works.
    private static bool Drain(IAsyncInfo op, int ms, out string error)
    {
        error = null;
        var deadline = DateTime.UtcNow.AddMilliseconds(ms);
        while (op.Status == AsyncStatus.Started)
        {
            if (DateTime.UtcNow > deadline)
            {
                error = "timeout after " + ms + "ms";
                return false;
            }
            Thread.Sleep(25);
        }
        if (op.Status == AsyncStatus.Error)
        {
            error = "async error 0x" + op.ErrorCode.HResult.ToString("x8");
            return false;
        }
        if (op.Status == AsyncStatus.Canceled)
        {
            error = "canceled";
            return false;
        }
        return true;
    }

    // Reflection on purpose: the projection this file compiles against exposes
    // Notification.Visual but not Notification.Content, and the exact member
    // names differ between OS builds (measured). Reading them reflectively keeps
    // one binary working on both and makes a missing member a visible
    // "no text" instead of a MissingMethodException at startup.
    private static object Prop(object o, string name)
    {
        if (o == null) return null;
        try
        {
            var pi = o.GetType().GetProperty(name, BindingFlags.Public | BindingFlags.Instance);
            return pi == null ? null : pi.GetValue(o, null);
        }
        catch { return null; }
    }

    private static string Text(object o)
    {
        var v = Prop(o, "Text");
        return v as string;
    }

    private static void Clamp(ref string s, int max)
    {
        if (s == null) { s = ""; return; }
        s = s.Trim();
        if (s.Length > max) s = s.Substring(0, max);
    }

    // Title = first text element, body = the rest joined with newlines. A toast
    // whose binding is not ToastGeneric (legacy template) has no text elements
    // we can trust, so it degrades to "no text" - the row then shows the app
    // display name only, never an invented string.
    private static void ReadToastText(UserNotification n, out string title, out string body)
    {
        title = "";
        body = "";
        var visual = Prop(Prop(n, "Notification"), "Visual");
        if (visual == null) return;
        var bindings = Prop(visual, "Bindings") as IEnumerable;
        if (bindings == null) return;
        var parts = new List<string>();
        foreach (var b in bindings)
        {
            if (b == null) continue;
            var template = Prop(b, "Template") as string;
            if (template != null && template.Length > 0 &&
                !template.Equals("ToastGeneric", StringComparison.Ordinal)) continue;
            object texts = null;
            try
            {
                var mi = b.GetType().GetMethod("GetTextElements",
                                               BindingFlags.Public | BindingFlags.Instance);
                if (mi != null) texts = mi.Invoke(b, null);
            }
            catch { }
            var list = texts as IEnumerable;
            if (list == null) continue;
            foreach (var t in list)
            {
                var s = Text(t);
                if (!string.IsNullOrEmpty(s)) parts.Add(s.Trim());
            }
            if (parts.Count > 0) break;  // the first usable binding is the content
        }
        if (parts.Count == 0) return;
        title = parts[0];
        if (parts.Count > 1) body = string.Join("\n", parts.GetRange(1, parts.Count - 1).ToArray());
        Clamp(ref title, kMaxTitleChars);
        Clamp(ref body, kMaxBodyChars);
    }

    private static string PollJson()
    {
        var sb = new StringBuilder();
        try
        {
            var listener = UserNotificationListener.Current;
            var access = (int)listener.GetAccessStatus();
            sb.Append("{\"ok\":true,\"access\":").Append(access);
            if (access != 1)  // not Allowed: no enumeration, and say so honestly
            {
                sb.Append(",\"count\":0,\"items\":[]}");
                return sb.ToString();
            }
            var op = listener.GetNotificationsAsync(NotificationKinds.Toast);
            string err;
            if (!Drain(op, 8000, out err))
                return "{\"ok\":false,\"error\":" + J("enumerate " + err) + "}";
            var notifs = op.GetResults();
            // Newest first, capped: the history panel shows a bounded list and
            // the dock only ever needs the head.
            var rows = new List<string>();
            int count = 0;
            for (int i = notifs.Count - 1; i >= 0 && count < kMaxItems; --i, ++count)
            {
                var n = notifs[i];
                if (n == null) continue;
                string title, body;
                ReadToastText(n, out title, out body);
                var app = Prop(n, "AppInfo");
                var display = Prop(app, "DisplayInfo");
                string appName = (Prop(display, "DisplayName") as string) ?? "";
                string aumid = (Prop(app, "AppUserModelId") as string) ?? "";
                Clamp(ref appName, 120);
                Clamp(ref aumid, 200);
                long created = 0;
                var ct = Prop(n, "CreationTime");
                if (ct is DateTimeOffset)
                    created = ((DateTimeOffset)ct).ToUnixTimeSeconds();
                rows.Add("{\"id\":" + n.Id +
                         ",\"aumid\":" + J(aumid) +
                         ",\"app\":" + J(appName) +
                         ",\"title\":" + J(title) +
                         ",\"body\":" + J(body) +
                         ",\"created\":" + created + "}");
            }
            sb.Append(",\"count\":").Append(notifs.Count)
              .Append(",\"items\":[").Append(string.Join(",", rows.ToArray())).Append("]}");
        }
        catch (Exception ex)
        {
            return "{\"ok\":false,\"error\":" + J(ex.GetType().Name + ": " + ex.Message) + "}";
        }
        return sb.ToString();
    }

    private static string ClearAll()
    {
        try
        {
            UserNotificationListener.Current.ClearNotifications();
            return "{\"ok\":true}";
        }
        catch (Exception ex)
        {
            return "{\"ok\":false,\"error\":" + J(ex.Message) + "}";
        }
    }

    private static string RemoveOne(uint id)
    {
        try
        {
            UserNotificationListener.Current.RemoveNotification(id);
            return "{\"ok\":true}";
        }
        catch (Exception ex)
        {
            return "{\"ok\":false,\"error\":" + J(ex.Message) + "}";
        }
    }

    private static void Emit(string line)
    {
        try
        {
            Console.Out.WriteLine(line);
            Console.Out.Flush();
        }
        catch { }
    }

    private static void PushThrottled()
    {
        lock (gate)
        {
            var now = DateTime.UtcNow;
            if ((now - lastPush).TotalMilliseconds < 250) return;
            lastPush = now;
        }
        Emit("{\"push\":\"notifications\"}");
    }

    private static int Main()
    {
        mainPid = Process.GetCurrentProcess().Id;
        try
        {
            Console.InputEncoding = Encoding.UTF8;
            Console.OutputEncoding = new UTF8Encoding(false);
        }
        catch { }

        var listener = UserNotificationListener.Current;
        int access = (int)listener.GetAccessStatus();
        // RequestAccessAsync is only needed when the user has never answered.
        // It can raise a system consent dialog, so it is attempted once and a
        // failure is reported - never silently treated as allowed.
        if (access == 0)
        {
            try
            {
                var req = listener.RequestAccessAsync();
                string err;
                if (Drain(req, 12000, out err)) access = (int)req.GetResults();
            }
            catch { }
        }

        if (access == 1)
        {
            try
            {
                listener.NotificationChanged += (s, e) => PushThrottled();
            }
            catch { }
        }

        Emit("{\"ok\":true,\"sidecar\":\"ready\",\"pid\":" + mainPid +
             ",\"access\":" + access + "}");

        Stream stdin = Console.OpenStandardInput();
        var reader = new StreamReader(stdin, Encoding.UTF8);
        while (true)
        {
            string line;
            try { line = reader.ReadLine(); }
            catch { break; }
            if (line == null) break;  // pipe closed: dock is gone, exit with it
            line = line.Trim();
            if (line.Length == 0) continue;
            if (line == "quit") break;
            if (line == "poll") { Emit(PollJson()); continue; }
            if (line == "clear") { Emit(ClearAll()); continue; }
            if (line.StartsWith("remove ", StringComparison.Ordinal))
            {
                uint id;
                if (uint.TryParse(line.Substring(7).Trim(), out id)) { Emit(RemoveOne(id)); continue; }
            }
            Emit("{\"ok\":false,\"error\":\"bad command\"}");
        }
        return 0;
    }
}
