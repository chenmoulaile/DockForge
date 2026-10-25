# DockForge T-065 spike (DoD #1): is UserNotificationListener (WinRT) viable for
# per-app unread counts, and which target apps (WeChat/QQ) are actually visible?
#
# Official .NET projection path: this script compiles a tiny C# helper with
# Add-Type (PowerShell 5.1's own System.Runtime.WindowsRuntime projection) and
# runs it in-process. The plain-PowerShell attempt failed because the runtime
# returned a raw System.__ComObject for IAsyncOperation (no projection wrapper
# for management interfaces in PS 5.1) - recorded in the log.
#
# Answers:
#   A. Can the runtime class be activated outside an MSIX container?
#   B. Does RequestAccessAsync succeed (or what exactly denies it)?
#   C. What does GetNotificationsAsync(Toast) return, aggregated by AppId?
#   D. ABI declaration order of IUserNotificationListener (recorded for spike.cpp).
#
# NOTE: first use of RequestAccessAsync pops a system consent dialog for an
# unpackaged app. Unattended runs wait 12s then report TIMEOUT; that state is
# part of the recorded result.

$ErrorActionPreference = 'Continue'
Write-Host '=== T-065 UserNotificationListener probe ==='
Write-Host ("PS version : " + $PSVersionTable.PSVersion)
Write-Host ("OS         : " + [Environment]::OSVersion.VersionString)

$src = @'
using System;
using System.Collections.Generic;
using System.Text;
using System.Threading.Tasks;
using Windows.UI.Notifications;
using Windows.UI.Notifications.Management;

public static class NotifProbe
{
    private static string Status(Task t) { return t.IsCompleted ? "completed" : (t.IsFaulted ? "faulted" : (t.IsCanceled ? "canceled" : "timeout")); }

    public static string Run()
    {
        var sb = new StringBuilder();
        // A. activation
        UserNotificationListener listener;
        try {
            listener = UserNotificationListener.Current;
            sb.AppendLine("A. activate factory    : OK, Current non-null = " + (listener != null));
        } catch (Exception ex) {
            sb.AppendLine("A. activate factory    : FAILED 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
            return sb.ToString();
        }

        // B. RequestAccessAsync (may pop the system consent dialog)
        sb.AppendLine("--- B. RequestAccessAsync (12s wait) ---");
        UserNotificationListenerAccessStatus access = UserNotificationListenerAccessStatus.Unspecified;
        try {
            var op = listener.RequestAccessAsync();
            var task = op.AsTask();
            bool done = task.Wait(12000);
            if (done && !task.IsFaulted) {
                access = op.GetResults();
                sb.AppendLine("   access = " + access);
            } else if (task.IsFaulted) {
                sb.AppendLine("   access = FAULTED " + task.Exception.GetBaseException().Message);
            } else {
                sb.AppendLine("   access = <12s timeout while Started - consent dialog unanswered?>");
            }
        } catch (Exception ex) {
            sb.AppendLine("   access = THREW 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
        }

        // C. GetNotificationsAsync(Toast)
        sb.AppendLine("--- C. GetNotificationsAsync(Toast) ---");
        if (access == UserNotificationListenerAccessStatus.Allowed) {
            try {
                var op2 = listener.GetNotificationsAsync(NotificationKinds.Toast);
                var task2 = op2.AsTask();
                if (task2.Wait(10000) && !task2.IsFaulted) {
                    IReadOnlyList<UserNotification> notifs = op2.GetResults();
                    sb.AppendLine("   total notifications visible = " + notifs.Count);
                    var byApp = new SortedDictionary<string, int>();
                    foreach (var n in notifs) {
                        string appId = n.AppId;
                        if (!byApp.ContainsKey(appId)) byApp[appId] = 0;
                        byApp[appId]++;
                    }
                    foreach (var kv in byApp) sb.AppendLine("   AppId=" + kv.Key + " count=" + kv.Value);
                    bool wechat = false, qq = false;
                    foreach (var k in byApp.Keys) {
                        string lower = k.ToLowerInvariant();
                        if (lower.Contains("weixin") || lower.Contains("wechat")) wechat = true;
                        if (lower.Contains("qq") || lower.Contains("tencent")) qq = true;
                    }
                    sb.AppendLine("   WeChat visible  : " + wechat);
                    sb.AppendLine("   QQ visible      : " + qq);
                } else if (task2.IsFaulted) {
                    sb.AppendLine("   GetNotificationsAsync FAULTED " + task2.Exception.GetBaseException().Message);
                } else {
                    sb.AppendLine("   GetNotificationsAsync <10s timeout>");
                }
            } catch (Exception ex) {
                sb.AppendLine("   GetNotificationsAsync THREW 0x" + ex.HResult.ToString("X8") + " " + ex.Message);
            }
        } else {
            sb.AppendLine("   skipped: access not Allowed (see B)");
        }
        return sb.ToString();
    }
}
'@

$winmd = @(
    "$env:windir\System32\WinMetadata\Windows.UI.winmd",
    "$env:windir\System32\WinMetadata\Windows.Foundation.winmd",
    "$env:windir\System32\WinMetadata\Windows.ApplicationModel.winmd"
)
foreach ($w in $winmd) {
    if (-not (Test-Path $w)) { throw "missing winmd: $w" }
}
Add-Type -TypeDefinition $src -ReferencedAssemblies (@('System.Runtime.WindowsRuntime') + $winmd) -ErrorAction Stop
Write-Host ([NotifProbe]::Run())
Write-Host '--- D. projected interface method order (== ABI declaration order) ---'
# from the first run of this probe (kept for the record; used by spike.cpp):
Write-Host '[IUserNotificationListener]'
Write-Host '   RequestAccessAsync'
Write-Host '   GetAccessStatus'
Write-Host '   add_NotificationChanged'
Write-Host '   remove_NotificationChanged'
Write-Host '   GetNotificationsAsync'
Write-Host '   GetNotification'
Write-Host '   ClearNotifications'
Write-Host '   RemoveNotification'
Write-Host '=== probe end ==='
