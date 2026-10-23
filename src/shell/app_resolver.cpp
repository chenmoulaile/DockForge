// DockForge - shell item resolution (CONTRACTS.md C-08).
#include "shell/app_resolver.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>

#include <algorithm>

#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"

using Microsoft::WRL::ComPtr;

namespace dockforge::shell {
namespace {

bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix) {
    const size_t n = std::wcslen(suffix);
    if (text.size() < n) return false;
    return _wcsicmp(text.c_str() + (text.size() - n), suffix) == 0;
}

// IShellLinkW reports the target as stored on disk. Shortcuts created by the
// Windows installer commonly keep "%windir%\..." verbatim, which no API accepts.
std::wstring Expand(const wchar_t* raw) {
    if (!raw || !*raw) return {};
    if (!std::wcschr(raw, L'%')) return raw;
    wchar_t buffer[1024]{};
    const DWORD written = ExpandEnvironmentStringsW(raw, buffer, ARRAYSIZE(buffer));
    if (written == 0 || written > ARRAYSIZE(buffer)) return raw;
    return buffer;
}

}  // namespace

const char* ToString(ResolveError error) {
    switch (error) {
        case ResolveError::None: return "none";
        case ResolveError::EmptyPath: return "empty-path";
        case ResolveError::NotFound: return "not-found";
        case ResolveError::NotAShortcut: return "not-a-shortcut";
        case ResolveError::LinkLoadFailed: return "link-load-failed";
        case ResolveError::NoTarget: return "no-target";
        case ResolveError::InvalidAumid: return "invalid-aumid";
    }
    return "unknown";
}

const char* ToString(ParsingKind kind) {
    switch (kind) {
        case ParsingKind::ShortcutPath: return "shortcut-path";
        case ParsingKind::FilePath: return "file-path";
        case ParsingKind::PackagedAppId: return "packaged-app-id";
        case ParsingKind::DesktopAppId: return "desktop-app-id";
        case ParsingKind::NamespacePath: return "namespace-path";
        case ParsingKind::Unknown: return "unknown";
    }
    return "unknown";
}

bool HasShortcutExtension(const std::wstring& path) {
    return EndsWithNoCase(path, L".lnk");
}

bool LooksLikeAumid(const std::wstring& text) {
    const size_t bang = text.find(L'!');
    if (bang == std::wstring::npos || bang == 0 || bang + 1 >= text.size()) return false;
    // The family name must contain an underscore ("<Name>_<publisher hash>"), and
    // the application id must not contain a separator.
    return text.find(L'_') < bang && text.find(L'\\') == std::wstring::npos;
}

bool LooksLikeAppId(const std::wstring& text) {
    if (text.empty()) return false;
    if (LooksLikeAumid(text)) return true;
    // A bare desktop app id such as "Anysphere.Cursor" or "Chrome": no path
    // separator, no drive letter, and no trailing extension that would make it a
    // file name. The shell resolves these through AppsFolder only.
    if (text.find(L'\\') != std::wstring::npos) return false;
    if (text.find(L'/') != std::wstring::npos) return false;
    if (text.size() >= 2 && text[1] == L':') return false;
    return true;
}

ParsingKind ClassifyParsingName(const std::wstring& parsing) {
    if (parsing.empty()) return ParsingKind::Unknown;
    if (LooksLikeAumid(parsing)) return ParsingKind::PackagedAppId;
    if (HasShortcutExtension(parsing)) return ParsingKind::ShortcutPath;
    // A drive-qualified path is a real file. A GUID-prefixed child is relative to a
    // shell namespace folder, not to the desktop, so it must not be treated as one.
    if (parsing.size() >= 2 && parsing[1] == L':') return ParsingKind::FilePath;
    if (parsing[0] == L'{' && parsing.find(L'\\') != std::wstring::npos) {
        return ParsingKind::NamespacePath;
    }
    if (parsing.find(L'\\') != std::wstring::npos) return ParsingKind::FilePath;
    return ParsingKind::DesktopAppId;
}

std::wstring TitleFromPath(const std::wstring& path) {
    if (path.empty()) return {};
    const size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

std::wstring DirectoryPart(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return {};
    // "C:\" is a drive root, so it has no parent; "C:\file.exe" lives in "C:\".
    if (slash == 2 && path.size() >= 2 && path[1] == L':') {
        return path.size() == 3 ? std::wstring{} : path.substr(0, 3);
    }
    return path.substr(0, slash);
}

std::wstring ShellNamespacePath(const std::wstring& identity) {
    if (identity.empty()) return {};
    const wchar_t* prefix = L"shell:AppsFolder\\";
    // Already qualified: a caller passing an icon_source back in must not get a
    // double prefix, which the shell rejects with E_INVALIDARG.
    if (_wcsnicmp(identity.c_str(), prefix, 16) == 0) return identity;
    return std::wstring(prefix) + identity;
}

std::wstring IconSourceFor(const AppEntry& entry) {
    if (!entry.icon_source.empty()) return entry.icon_source;
    if ((entry.kind == AppKind::Uwp || entry.kind == AppKind::ShellItem) &&
        !entry.aumid.empty()) {
        return ShellNamespacePath(entry.aumid);
    }
    if (entry.kind == AppKind::NamespaceItem && !entry.namespace_parsing.empty()) {
        return ShellNamespacePath(entry.namespace_parsing);
    }
    if (!entry.icon_path.empty()) return entry.icon_path;
    if (!entry.target_path.empty()) return entry.target_path;
    return {};
}

std::wstring ExpandEnvironment(const std::wstring& text) { return Expand(text.c_str()); }

ResolveError ResolveShortcut(const std::wstring& lnk_path, AppEntry& out) {
    out = AppEntry{};
    if (lnk_path.empty()) return ResolveError::EmptyPath;
    if (!HasShortcutExtension(lnk_path)) return ResolveError::NotAShortcut;

    const DWORD attrs = GetFileAttributesW(lnk_path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return ResolveError::NotFound;
    }

    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&link));
    if (FAILED(hr)) {
        DF_WARN("shell", "CoCreateInstance(CLSID_ShellLink) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        return ResolveError::LinkLoadFailed;
    }

    ComPtr<IPersistFile> file;
    hr = link.As(&file);
    if (FAILED(hr)) return ResolveError::LinkLoadFailed;

    hr = file->Load(lnk_path.c_str(), STGM_READ);
    if (FAILED(hr)) {
        DF_WARN("shell", "IPersistFile::Load(%s) failed: 0x%08lX",
                core::WideToUtf8(lnk_path).c_str(), static_cast<unsigned long>(hr));
        return ResolveError::LinkLoadFailed;
    }

    wchar_t target[MAX_PATH * 2]{};
    WIN32_FIND_DATAW find_data{};
    hr = link->GetPath(target, ARRAYSIZE(target), &find_data, SLGP_RAWPATH);
    if (FAILED(hr)) return ResolveError::NoTarget;

    wchar_t args[1024]{};
    link->GetArguments(args, ARRAYSIZE(args));

    wchar_t icon[1024]{};
    int icon_index = 0;
    link->GetIconLocation(icon, ARRAYSIZE(icon), &icon_index);

    wchar_t workdir[MAX_PATH * 2]{};
    link->GetWorkingDirectory(workdir, ARRAYSIZE(workdir));

    out.kind = AppKind::Shortcut;
    out.target_path = Expand(target);
    out.arguments = args;
    out.icon_path = Expand(icon);
    out.icon_index = icon_index;
    out.working_dir = Expand(workdir);
    out.display_name = TitleFromPath(lnk_path);

    // A shortcut to an uninstaller or a document still needs a working dir, and an
    // empty GetWorkingDirectory would make CreateProcess inherit ours.
    if (out.working_dir.empty()) out.working_dir = DirectoryPart(out.target_path);
    if (out.target_path.empty()) return ResolveError::NoTarget;

    out.icon_source = !out.icon_path.empty() ? out.icon_path : out.target_path;
    return ResolveError::None;
}

ResolveError ResolvePath(const std::wstring& path, AppEntry& out) {
    out = AppEntry{};
    if (path.empty()) return ResolveError::EmptyPath;

    if (HasShortcutExtension(path)) return ResolveShortcut(path, out);

    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return ResolveError::NotFound;
    }

    out.kind = AppKind::Executable;
    out.target_path = path;
    out.working_dir = DirectoryPart(path);
    out.display_name = TitleFromPath(path);
    out.icon_source = path;
    return ResolveError::None;
}

AppEntry MakeUwpEntry(const std::wstring& display_name, const std::wstring& aumid) {
    AppEntry entry;
    entry.kind = AppKind::Uwp;
    entry.aumid = aumid;
    entry.display_name = display_name.empty() ? aumid : display_name;
    entry.icon_source = L"shell:AppsFolder\\" + aumid;
    return entry;
}

AppEntry MakeShellItemEntry(const std::wstring& display_name, const std::wstring& app_id) {
    AppEntry entry;
    entry.kind = AppKind::ShellItem;
    entry.aumid = app_id;
    entry.display_name = display_name.empty() ? app_id : display_name;
    entry.icon_source = ShellNamespacePath(app_id);
    return entry;
}

AppEntry MakeNamespaceEntry(const std::wstring& display_name, const std::wstring& parsing) {
    AppEntry entry;
    entry.kind = AppKind::NamespaceItem;
    entry.namespace_parsing = parsing;
    entry.display_name = display_name.empty() ? parsing : display_name;
    entry.icon_source = ShellNamespacePath(parsing);
    return entry;
}

ResolveError ResolveParsingName(const std::wstring& parsing, const std::wstring& display_name,
                                AppEntry& out) {
    out = AppEntry{};
    switch (ClassifyParsingName(parsing)) {
        case ParsingKind::PackagedAppId:
            out = MakeUwpEntry(display_name, parsing);
            return ResolveError::None;
        case ParsingKind::DesktopAppId:
            // No executable path exists for these, so the AUMID is the identity.
            // The shell already proved the id exists by listing it.
            out = MakeShellItemEntry(display_name, parsing);
            return ResolveError::None;
        case ParsingKind::NamespacePath:
            // Icons and launching both work through the shell:AppsFolder prefix;
            // the bare "{GUID}\name" form is rejected by SHCreateItemFromParsingName.
            out = MakeNamespaceEntry(display_name, parsing);
            return ResolveError::None;
        case ParsingKind::ShortcutPath: {
            const ResolveError error = ResolveShortcut(parsing, out);
            if (error == ResolveError::None && !display_name.empty()) {
                out.display_name = display_name;
            }
            return error;
        }
        case ParsingKind::FilePath: {
            const ResolveError error = ResolvePath(parsing, out);
            if (error == ResolveError::None && !display_name.empty()) {
                out.display_name = display_name;
            }
            return error;
        }
        case ParsingKind::Unknown:
            return ResolveError::EmptyPath;
    }
    return ResolveError::EmptyPath;
}

ResolveError ResolveAumid(const std::wstring& aumid, AppEntry& out) {
    out = AppEntry{};
    if (!LooksLikeAumid(aumid)) return ResolveError::InvalidAumid;

    // This call is the only real validation available: the shell namespace lookup
    // fails with ERROR_FILE_NOT_FOUND for a package that is not installed.
    const std::wstring parsing = L"shell:AppsFolder\\" + aumid;
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) {
        DF_DEBUG("shell", "AUMID %s not installed (0x%08lX)", core::WideToUtf8(aumid).c_str(),
                 static_cast<unsigned long>(hr));
        return ResolveError::InvalidAumid;
    }

    std::wstring name;
    ComPtr<IShellItem2> item2;
    if (SUCCEEDED(item.As(&item2))) {
        PWSTR display = nullptr;
        if (SUCCEEDED(item2->GetString(PKEY_ItemNameDisplay, &display)) && display) {
            name = display;
            CoTaskMemFree(display);
        }
    }

    out = MakeUwpEntry(name, aumid);
    return ResolveError::None;
}

}  // namespace dockforge::shell
