// DockForge - one launchable thing the Dock can show.
// Contract: CONTRACTS.md C-08.
#pragma once

#include <string>

namespace dockforge::shell {

enum class AppKind {
    Shortcut,       // .lnk resolved through IShellLinkW
    Executable,     // a bare .exe (or other shell-item path)
    Uwp,            // packaged app, launched by AppUserModelID
    ShellItem,      // registered Win32 app with an AUMID but no path
    NamespaceItem,  // shell-namespace child, identity is "{GUID}\name" (see below)
    Folder,         // a directory pinned by an OLE drop (T-014)
    Unknown,
};

const char* ToString(AppKind kind);

// Everything needed to (a) draw a tile and (b) start the app, resolved once at
// load time so the click path never touches the shell again.
struct AppEntry {
    AppKind kind = AppKind::Unknown;

    // Localized label shown under the icon. For UWP this comes from the shell
    // namespace enumeration, never from SIGDN_NORMALDISPLAY (which is empty).
    std::wstring display_name;

    // Executable to start. Empty for UWP entries. Environment variables are
    // already expanded here: IShellLinkW::GetPath returns
    // "%windir%\system32\control.exe" for several real Start-menu shortcuts, and
    // both CreateProcess and SHCreateItemFromParsingName reject that form.
    std::wstring target_path;

    // Command line arguments exactly as stored in the .lnk (may be empty).
    std::wstring arguments;

    // Working directory from the .lnk; falls back to the target's directory.
    std::wstring working_dir;

    // AppUserModelID for packaged entries (Uwp) and for Win32 apps that registered
    // one with the Start menu (ShellItem), e.g.
    // "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App" or "Anysphere.Cursor".
    // Empty otherwise.
    std::wstring aumid;

    // Shell-namespace parsing name for NamespaceItem entries, e.g.
    // "{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\cmd.exe". It is only meaningful
    // relative to AppsFolder, so every use must go through ShellNamespacePath().
    std::wstring namespace_parsing;

    // What to hand IShellItemImageFactory. Either a filesystem path or
    // "shell:AppsFolder\<identity>". Kept separate from target_path because a UWP
    // app has no path and a .lnk's icon can live in a different DLL.
    std::wstring icon_source;

    // Icon resource from IShellLinkW::GetIconLocation, already expanded. May be
    // empty, in which case icon_source falls back to target_path. A negative
    // index means "resource ID" rather than "ordinal", so it is kept verbatim.
    std::wstring icon_path;
    int icon_index = 0;

    bool launchable() const {
        switch (kind) {
            case AppKind::Uwp:
            case AppKind::ShellItem:
                return !aumid.empty();
            case AppKind::NamespaceItem:
                return !namespace_parsing.empty();
            default:
                // Folder is included: it has a target_path and is started through
                // ShellExecuteEx rather than CreateProcess (a folder is not an
                // executable, and CreateProcess on one fails with ACCESS_DENIED).
                return !target_path.empty();
        }
    }
};

// "shell:AppsFolder\<identity>": the only string the shell accepts for an app that
// has no filesystem path. Used for both icons and ShellExecuteEx.
std::wstring ShellNamespacePath(const std::wstring& identity);

}  // namespace dockforge::shell
