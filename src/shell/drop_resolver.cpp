#include "shell/drop_resolver.h"

#include <windows.h>

#include <cwctype>

namespace dockforge::shell {
namespace {

bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix) {
    const std::size_t suffix_len = wcslen(suffix);
    if (text.size() < suffix_len) return false;
    const std::size_t offset = text.size() - suffix_len;
    for (std::size_t i = 0; i < suffix_len; ++i) {
        if (std::towlower(text[offset + i]) != std::towlower(suffix[i])) return false;
    }
    return true;
}

}  // namespace

const char* ToString(DropKind kind) {
    switch (kind) {
        case DropKind::None: return "none";
        case DropKind::Shortcut: return "shortcut";
        case DropKind::Executable: return "executable";
        case DropKind::Folder: return "folder";
        case DropKind::Unsupported: return "unsupported";
    }
    return "none";
}

bool IsDockable(DropKind kind) {
    // A folder is accepted: the dock is a launcher, and "open this project folder"
    // is a legitimate thing to pin. Documents are not - they belong in the document,
    // not on a launcher, and pinning one would produce a tile that opens an editor.
    return kind == DropKind::Shortcut || kind == DropKind::Executable || kind == DropKind::Folder;
}

DropKind ClassifyDroppedPath(const std::wstring& path, unsigned long attributes) {
    if (path.empty()) return DropKind::None;
    if (attributes == INVALID_FILE_ATTRIBUTES) return DropKind::None;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return DropKind::Folder;
    if (EndsWithNoCase(path, L".lnk")) return DropKind::Shortcut;
    if (EndsWithNoCase(path, L".exe") || EndsWithNoCase(path, L".com") ||
        EndsWithNoCase(path, L".bat") || EndsWithNoCase(path, L".cmd")) {
        return DropKind::Executable;
    }
    return DropKind::Unsupported;
}

DropKind ClassifyDroppedPath(const std::wstring& path) {
    if (path.empty()) return DropKind::None;
    return ClassifyDroppedPath(path, GetFileAttributesW(path.c_str()));
}

DropKind ClassifyDroppedPaths(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return DropKind::None;
    DropKind result = DropKind::None;
    for (const auto& path : paths) {
        const DropKind kind = ClassifyDroppedPath(path);
        if (!IsDockable(kind)) return kind == DropKind::None ? DropKind::None : DropKind::Unsupported;
        // The reported kind is the first one so the caller can log what it saw;
        // acceptance is the conjunction, which is why the loop does not stop early on
        // a dockable kind.
        if (result == DropKind::None) result = kind;
    }
    return result;
}

ResolveError ResolveDroppedPath(const std::wstring& path, AppEntry& out) {
    out = AppEntry{};
    if (path.empty()) return ResolveError::EmptyPath;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return ResolveError::NotFound;

    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        out.kind = AppKind::Folder;
        out.target_path = path;
        // The icon source is the folder itself: the shell returns the standard
        // folder icon, which is what Explorer shows, and no .lnk is involved.
        out.icon_source = path;
        // Explorer's own "open" verb, so a folder behaves exactly like double-
        // clicking it. Using the shell rather than CreateProcess matters: a folder is
        // not an executable and CreateProcess on one fails with ERROR_ACCESS_DENIED.
        out.display_name = path;
        while (!out.display_name.empty() &&
               (out.display_name.back() == L'\\' || out.display_name.back() == L'/')) {
            out.display_name.pop_back();
        }
        const std::size_t slash = out.display_name.find_last_of(L"\\/");
        if (slash != std::wstring::npos) out.display_name = out.display_name.substr(slash + 1);
        if (out.display_name.empty()) out.display_name = path;
        return ResolveError::None;
    }

    // Everything else reuses the Start-menu path resolver: a dropped .lnk must be
    // expanded (raw paths carry %windir%) and a dropped .exe must get a working
    // directory, and both of those already live in one place.
    return ResolvePath(path, out);
}

}  // namespace dockforge::shell
