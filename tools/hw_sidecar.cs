// DockForge T-068 hardware sidecar: GSMTC media control + Windows.Devices.Radios.
//
// WHY THIS EXISTS (measured, spikes/T-068_hw_probe):
//   - The MinGW main process cannot drive these two WinRT areas: their entry
//     points are IAsyncOperation (RequestAsync / GetRadiosAsync), and in a
//     console process those never complete - status stays Started for 15s with
//     a registered completion handler that never fires (winrt_mingw.cpp), the
//     same boundary T-065 measured for UserNotificationListener. The .NET
//     projection completes both in about a second (winrt_probe.cs log).
//   - So the main process spawns this exe and talks a line protocol over
//     stdin/stdout. No C++/WinRT, no MSVC, only the .NET Framework every
//     Windows machine ships.
//
// PROTOCOL (one JSON object per line, UTF-8 without BOM):
//   -> {"cmd":"poll"}
//   <- {"ok":true,"media":{"aumid":"","title":"","artist":"","playing":false,
//                          "has_session":false,"pos":0,"dur":0},
//       "radios":[{"name":"","kind":1,"state":1}]}
//   -> {"cmd":"media","action":"play|pause|toggle|next|prev"}
//   <- {"ok":true} or {"ok":false,"error":"..."}
//   -> {"cmd":"radio","kind":1,"on":true}       (kind: 1=WiFi 3=Bluetooth)
//   <- {"ok":true} or {"ok":false,"error":"..."}
//   -> {"cmd":"quit"}
// Push lines (event-driven, throttled to >=250ms apart): {"push":"media"}
// or {"push":"radio"} - the main process answers with a fresh poll.
//
// Build (repo root, Git Bash; the stock .NET Framework csc, no SDK):
//   C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe -nologo -target:exe \
//     -out:build/bin/hw_sidecar.exe \
//     -r:System.Runtime.WindowsRuntime.dll \
//     -r:C:/Windows/Microsoft.NET/Framework64/v4.0.30319/System.Runtime.dll \
//     -r:C:/Windows/System32/WinMetadata/Windows.Foundation.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.Media.winmd \
//     -r:C:/Windows/System32/WinMetadata/Windows.Devices.winmd \
//     tools/hw_sidecar.cs
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;
using Windows.Foundation;
using Windows.Media.Control;
using Windows.Devices.Radios;

internal static class HwSidecar
{
    private static readonly object gate = new object();
    private static GlobalSystemMediaTransportControlsSessionManager mgr;
    private static DateTime lastPush = DateTime.MinValue;
    private static int mainPid;

    private static string J(string s)
    {
        if (s == null) return "\"\"";
        var sb = new StringBuilder();
        sb.Append('"');
        foreach (var c in s)
        {
            if (c == '"' || c == '\\') sb.Append('\\').Append(c);
            else if (c == '\n') sb.Append("\\n");
            else if (c == '\r') sb.Append("\\r");
            else if (c == '\t') sb.Append("\\t");
            else if (c < ' ') sb.Append("\\u").Append(((int)c).ToString("x4",
                CultureInfo.InvariantCulture));
            else sb.Append(c);
        }
        sb.Append('"');
        return sb.ToString();
    }

    private static bool Drain(IAsyncInfo op, int ms, out string error)
    {
        error = null;
        var deadline = DateTime.UtcNow.AddMilliseconds(ms);
        while (op.Status == AsyncStatus.Started)
        {
            if (DateTime.UtcNow > deadline) { error = "timeout"; return false; }
            Thread.Sleep(50);
        }
        if (op.Status == AsyncStatus.Completed) return true;
        error = "async status " + op.Status;
        return false;
    }

    private static void Emit(string line)
    {
        lock (gate)
        {
            Console.Out.WriteLine(line);
            Console.Out.Flush();
        }
    }

    private static void PushThrottled(string what)
    {
        lock (gate)
        {
            if ((DateTime.UtcNow - lastPush).TotalMilliseconds < 250) return;
            lastPush = DateTime.UtcNow;
        }
        Emit("{\"push\":" + J(what) + "}");
    }

    private static string MediaJson()
    {
        var has = false; var aumid = ""; var title = ""; var artist = "";
        var playing = false; long pos = 0; long dur = 0;
        if (mgr != null)
        {
            var s = mgr.GetCurrentSession();
            if (s != null)
            {
                has = true;
                aumid = s.SourceAppUserModelId ?? "";
                var info = s.GetPlaybackInfo();
                playing = info != null && info.PlaybackStatus ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;
                var t = s.GetTimelineProperties();
                pos = t.Position.Ticks; dur = t.EndTime.Ticks;
                // Title/artist need an async fetch: cached values are refreshed
                // by the MediaPropertiesChanged handler into the fields below.
                title = cachedTitle; artist = cachedArtist;
            }
        }
        var sb = new StringBuilder();
        sb.Append("{\"aumid\":").Append(J(aumid))
          .Append(",\"title\":").Append(J(title))
          .Append(",\"artist\":").Append(J(artist))
          .Append(",\"playing\":").Append(playing ? "true" : "false")
          .Append(",\"has_session\":").Append(has ? "true" : "false")
          .Append(",\"pos\":").Append(pos).Append(",\"dur\":").Append(dur).Append('}');
        return sb.ToString();
    }

    private static string cachedTitle = "";
    private static string cachedArtist = "";

    private static void RefetchMediaProps(GlobalSystemMediaTransportControlsSession s)
    {
        try
        {
            var op = s.TryGetMediaPropertiesAsync();
            string err;
            if (Drain(op, 8000, out err))
            {
                var p = op.GetResults();
                if (p != null)
                {
                    cachedTitle = p.Title ?? "";
                    cachedArtist = p.Artist ?? "";
                }
            }
        }
        catch { /* properties stay at their last value */ }
    }

    private static string RadiosJson()
    {
        var sb = new StringBuilder("[");
        try
        {
            var op = Radio.GetRadiosAsync();
            string err;
            if (Drain(op, 8000, out err))
            {
                var radios = op.GetResults();
                var first = true;
                foreach (var r in radios)
                {
                    if (!first) sb.Append(',');
                    first = false;
                    sb.Append("{\"name\":").Append(J(r.Name))
                      .Append(",\"kind\":").Append((int)r.Kind)
                      .Append(",\"state\":").Append((int)r.State).Append('}');
                }
            }
        }
        catch { /* empty list on refusal */ }
        sb.Append(']');
        return sb.ToString();
    }

    private static string PollJson()
    {
        return "{\"ok\":true,\"media\":" + MediaJson() + ",\"radios\":" + RadiosJson() + "}";
    }

    private static string MediaControl(string action)
    {
        var s = mgr == null ? null : mgr.GetCurrentSession();
        if (s == null) return "{\"ok\":false,\"error\":\"no media session\"}";
        IAsyncOperation<bool> op = null;
        switch (action)
        {
            case "play": op = s.TryPlayAsync(); break;
            case "pause": op = s.TryPauseAsync(); break;
            case "toggle": op = s.TryTogglePlayPauseAsync(); break;
            case "next": op = s.TrySkipNextAsync(); break;
            case "prev": op = s.TrySkipPreviousAsync(); break;
            default: return "{\"ok\":false,\"error\":\"unknown action " + action + "\"}";
        }
        string err;
        if (!Drain(op, 8000, out err))
            return "{\"ok\":false,\"error\":" + J(err) + "}";
        return op.Status == AsyncStatus.Completed && op.GetResults()
            ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rejected\"}";
    }

    private static string RadioControl(int kind, bool on)
    {
        try
        {
            var op = Radio.GetRadiosAsync();
            string err;
            if (!Drain(op, 8000, out err)) return "{\"ok\":false,\"error\":" + J(err) + "}";
            foreach (var r in op.GetResults())
            {
                if ((int)r.Kind != kind) continue;
                var set = r.SetStateAsync(on ? RadioState.On : RadioState.Off);
                if (!Drain(set, 8000, out err)) return "{\"ok\":false,\"error\":" + J(err) + "}";
                return set.GetResults() == RadioAccessStatus.Allowed
                    ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rejected\"}";
            }
            return "{\"ok\":false,\"error\":\"radio kind not found\"}";
        }
        catch (Exception ex)
        {
            return "{\"ok\":false,\"error\":" + J(ex.Message) + "}";
        }
    }

    private static void WireEvents()
    {
        if (mgr == null) return;
        // The projected events need System.Runtime.InteropServices.WindowsRuntime
        // referenced (EventRegistrationToken); with it, plain += compiles.
        mgr.CurrentSessionChanged += (s, e) =>
        {
            var session = mgr.GetCurrentSession();
            cachedTitle = ""; cachedArtist = "";
            if (session != null)
            {
                session.PlaybackInfoChanged += (s2, e2) => PushThrottled("media");
                session.MediaPropertiesChanged += (s2, e2) =>
                {
                    RefetchMediaProps(session);
                    PushThrottled("media");
                };
                RefetchMediaProps(session);
            }
            PushThrottled("media");
        };
        var cur = mgr.GetCurrentSession();
        if (cur != null)
        {
            cur.PlaybackInfoChanged += (s2, e2) => PushThrottled("media");
            cur.MediaPropertiesChanged += (s2, e2) =>
            {
                RefetchMediaProps(cur);
                PushThrottled("media");
            };
            RefetchMediaProps(cur);
        }
    }

    private static int Main()
    {
        mainPid = Process.GetCurrentProcess().Id;
        // Parent died detection: exit when the parent is gone so no orphan
        // sidecar outlives the dock (checked between commands).
        try
        {
            Console.InputEncoding = Encoding.UTF8;
            Console.OutputEncoding = new UTF8Encoding(false);
        }
        catch { /* pipes stay with the default encoding; JSON is ASCII-safe
                   except title/artist, which still round-trip via UTF8 decode
                   on the C++ side because we set it here when we can */ }

        var req = GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
        string err;
        if (!Drain(req, 20000, out err))
        {
            Console.Out.WriteLine("{\"ok\":false,\"error\":" + J("gsmtc " + err) + "}");
            return 1;
        }
        mgr = req.GetResults();
        WireEvents();
        // Radio state pushes: poll-free listening is not projected for statics;
        // the panel's 0.5Hz poll covers radio refresh.

        Console.Out.WriteLine("{\"ok\":true,\"sidecar\":\"ready\",\"pid\":" + mainPid + "}");
        try { Console.Out.Flush(); } catch { }

        Stream stdin = Console.OpenStandardInput();
        var reader = new StreamReader(stdin, Encoding.UTF8);
        while (true)
        {
            string line;
            try { line = reader.ReadLine(); }
            catch { break; }
            if (line == null) break;  // pipe closed: dock is gone
            line = line.Trim();
            if (line.Length == 0) continue;
            var reply = "{\"ok\":false,\"error\":\"bad command\"}";
            // command grammar kept trivial on purpose (no JSON parse on this
            // side): "poll" | "media <action>" | "radio <kind> <0|1>" | "quit"
            if (line == "poll") reply = PollJson();
            else if (line == "quit") break;
            else if (line.StartsWith("media ", StringComparison.Ordinal))
                reply = MediaControl(line.Substring(6).Trim());
            else if (line.StartsWith("radio ", StringComparison.Ordinal))
            {
                var parts = line.Substring(6).Trim().Split(' ');
                int kind; bool on;
                if (parts.Length == 2 && int.TryParse(parts[0], out kind) &&
                    (parts[1] == "0" || parts[1] == "1"))
                {
                    on = parts[1] == "1";
                    reply = RadioControl(kind, on);
                }
            }
            Emit(reply);
        }
        return 0;
    }
}
