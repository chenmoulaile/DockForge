// DockForge - which dock tile does a notification belong to (T-076).
//
// The banner's click and the C-22 pulse both need the same answer, so it is one
// pure function rather than two inline comparisons that could drift apart.
//
// Matching is deliberately loose in one direction and strict in the other:
//   * strict on the AUMID, because it is the only stable identity a toast
//     carries (measured: Windows.Defender.SecurityCenter,
//     Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows);
//   * loose on the display name, because Win32 toasts carry no AUMID at all
//     (measured: four of 63 notifications had an empty aumid) and the app name
//     the user sees in the toast is the app name the user pinned to the dock.
// A wrong pulse is cheap; a wrong click target is not, so the click path
// additionally requires the tile to be launchable.
#pragma once

#include "notifications/notification_types.h"

#include <cstddef>
#include <string>
#include <vector>

namespace dockforge::notifications {

// What a dock tile contributes to the match. Kept as a plain value so this
// header does not make the notification module depend on dock_view.
struct TileIdentity {
    std::wstring aumid;
    std::wstring display_name;
};

// Index of the tile a notification belongs to, or kNoTile.
inline constexpr int kNoTile = -1;

int FindTileForNotification(const NotificationItem& item, const std::vector<TileIdentity>& tiles);

// Lowercases ASCII only, matching shell::LowerAscii's rule (a locale fold would
// make the same toast match different tiles on different machines).
std::wstring LowerAsciiWide(const std::wstring& text);

}  // namespace dockforge::notifications
