// DockForge - notification centre types and the sidecar line parser (T-076).
//
// Everything here is pure: no windows, no processes, no WinRT. The parsing half
// is unit-tested against the exact lines the sidecar was measured to emit
// (evidence/T-076/spike_sidecar_protocol.log), so a protocol change shows up as
// a failing test rather than as an empty notification centre.
//
// PRIVACY (ADR-0025): these structs are the only place notification text ever
// lives. They are never serialised, never logged, and never written to disk -
// `body` in particular must not reach DF_LOG or the crash dump.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dockforge::notifications {

// Windows.UI.Notifications.Management.UserNotificationListenerAccessStatus.
// Unspecified means the user has never answered; the sidecar asks once, and a
// refusal is reported honestly instead of being treated as "no notifications".
enum class AccessStatus {
    Unspecified = 0,
    Allowed = 1,
    Denied = 2,
};

const char* ToString(AccessStatus status);

// One toast as the sidecar reported it. `id` is the listener's own UInt32 and
// was measured stable across enumerations, which is what makes dedup possible
// without hashing content.
struct NotificationItem {
    std::uint32_t id = 0;
    std::wstring aumid;      // may be empty (Win32 toasts often carry none)
    std::wstring app;        // AppInfo.DisplayInfo.DisplayName, may be empty
    std::wstring title;      // first toast text element, may be empty
    std::wstring body;       // remaining text elements joined by newline
    std::int64_t created_unix = 0;

    // What a row/banner shows as the sender. Never empty: an unnamed toast
    // still has to be attributable to something.
    std::wstring DisplayApp() const;
    // Headline for the banner: title, else the body's first line, else app.
    std::wstring Headline() const;
    // Body with the headline removed when the headline was the body's first
    // line; empty when there is nothing left to show.
    std::wstring Detail() const;
};

// One `poll` answer.
struct NotificationState {
    bool valid = false;              // the line was a well-formed poll answer
    AccessStatus access = AccessStatus::Unspecified;
    int total = 0;                   // how many the listener holds (before capping)
    std::vector<NotificationItem> items;  // newest first, capped by the sidecar
    std::wstring error;              // non-empty on a well-formed failure answer
};

// Parses one sidecar output line. Shapes:
//   {"ok":true,"access":1,"count":N,"items":[...]}  -> *out filled, valid=true
//   {"ok":true,"sidecar":"ready","pid":N,"access":A} -> valid=false, *ready=true
//   {"ok":true}                                     -> a bare command ack
//   {"ok":false,"error":"..."}                      -> *error filled
//   {"push":"notifications"}                        -> *is_push=true
// Returns false for anything unparsable (the caller drops the line).
bool ParseNotificationLine(const std::string& line, NotificationState* out, bool* is_push,
                           bool* ready, std::wstring* error);

}  // namespace dockforge::notifications
