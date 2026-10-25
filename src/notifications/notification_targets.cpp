// DockForge - which dock tile does a notification belong to (see the header).
#include "notifications/notification_targets.h"

#include <algorithm>

namespace dockforge::notifications {
namespace {

// Strips a package family name down to the part a toast's AUMID shares with a
// pinned app: "Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows"
// and "Microsoft.OutlookForWindows_8wekyb3d8bbwe" must compare equal, because the
// dock stores the AppUserModelId the shell handed it and the listener reports the
// full activation id.
std::wstring PackagePrefix(const std::wstring& aumid) {
    const std::size_t bang = aumid.find(L'!');
    return bang == std::wstring::npos ? aumid : aumid.substr(0, bang);
}

bool Contains(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty() || haystack.size() < needle.size()) return false;
    return haystack.find(needle) != std::wstring::npos;
}

}  // namespace

std::wstring LowerAsciiWide(const std::wstring& text) {
    std::wstring out = text;
    for (wchar_t& ch : out) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return out;
}

int FindTileForNotification(const NotificationItem& item, const std::vector<TileIdentity>& tiles) {
    const std::wstring aumid = LowerAsciiWide(item.aumid);
    const std::wstring app = LowerAsciiWide(item.DisplayApp());

    // Pass 1: exact AUMID. This is the identity the dock stores, so a hit here is
    // never a guess.
    if (!aumid.empty()) {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            if (LowerAsciiWide(tiles[i].aumid) == aumid) return static_cast<int>(i);
        }
        // Pass 2: the toast's activation id carries a "!App" suffix the dock's
        // stored AUMID may not. Compare the package prefix both ways so either
        // shape finds the other.
        const std::wstring toast_prefix = PackagePrefix(aumid);
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const std::wstring tile_aumid = LowerAsciiWide(tiles[i].aumid);
            if (tile_aumid.empty()) continue;
            const std::wstring tile_prefix = PackagePrefix(tile_aumid);
            if (tile_prefix == toast_prefix) return static_cast<int>(i);
        }
    }

    // Pass 3: display name. Only reached for Win32 toasts (no AUMID) and for
    // packaged apps whose AUMID the dock never learned. Substring rather than
    // equality because the toast says "Outlook" while the shortcut says
    // "Microsoft Outlook" as often as the other way round.
    if (!app.empty()) {
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const std::wstring name = LowerAsciiWide(tiles[i].display_name);
            if (name.empty()) continue;
            if (name == app) return static_cast<int>(i);
        }
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const std::wstring name = LowerAsciiWide(tiles[i].display_name);
            if (name.empty()) continue;
            if (Contains(name, app) || Contains(app, name)) return static_cast<int>(i);
        }
    }

    return kNoTile;
}

}  // namespace dockforge::notifications
