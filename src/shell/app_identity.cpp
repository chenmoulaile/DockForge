// DockForge - app identity implementation (CONTRACTS.md C-15a).
#include "shell/app_identity.h"

#include "core/paths.h"

namespace dockforge::shell {

std::string LowerAscii(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return text;
}

std::string IdentityOf(const AppEntry& app) {
    // AUMID first: for a packaged app it is the only stable identity - every
    // packaged window is hosted by ApplicationFrameHost.exe, so the path is shared
    // and would make two apps look identical (C-13b).
    if (!app.aumid.empty()) return LowerAscii(core::WideToUtf8(app.aumid));
    if (!app.namespace_parsing.empty()) {
        return LowerAscii(core::WideToUtf8(app.namespace_parsing));
    }
    if (!app.target_path.empty()) return LowerAscii(core::WideToUtf8(app.target_path));
    // Last resort: the display name. Two different apps could share one, which is
    // why it is last - but a tile with no identity at all could never be reordered
    // or hidden, and would come back after every restart.
    return LowerAscii(core::WideToUtf8(app.display_name));
}

}  // namespace dockforge::shell
