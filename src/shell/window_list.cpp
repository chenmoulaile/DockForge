#include "shell/window_list.h"

#include "core/paths.h"
#include "dock/tile_order.h"

#include <algorithm>

namespace dockforge::shell {
namespace {

std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), ::towlower);
    return text;
}

// The window-side identity, mirroring dock::IdentityOf's field order: AUMID
// first (the only stable identity for a packaged window, whose executable path
// is shared by every packaged app), then the executable path.
std::wstring IdentityOfWindow(const AppWindow& window) {
    if (!window.aumid.empty()) return Lowercase(window.aumid);
    if (!window.exe_path.empty()) return Lowercase(window.exe_path);
    return {};
}

}  // namespace

std::vector<AppWindow> WindowsForEntry(const AppEntry& entry,
                                       const std::vector<AppWindow>& windows) {
    // The tile's identity comes from the one implementation that owns it
    // (dock::IdentityOf, C-15a). A tile's identity is stable across scans, which
    // is what makes comparing identities survive a re-scan.
    const std::wstring identity = core::Utf8ToWide(dock::IdentityOf(entry));
    if (identity.empty()) {
        return {};
    }

    std::vector<AppWindow> matches;
    for (const AppWindow& window : windows) {
        if (IdentityOfWindow(window) == identity) {
            matches.push_back(window);
        }
    }
    return matches;
}

}  // namespace dockforge::shell
