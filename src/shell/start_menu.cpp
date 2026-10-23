// DockForge - Start-menu / AppsFolder enumeration (CONTRACTS.md C-08).
#include "shell/start_menu.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>

#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"
#include "shell/app_resolver.h"

using Microsoft::WRL::ComPtr;

namespace dockforge::shell {
namespace {

// Reads one display name from a child pidl. SHGDN_NORMAL is the label the Start
// menu shows; SHGDN_FORPARSING is the identity we key on.
std::wstring ChildName(IShellFolder* folder, PCUITEMID_CHILD child, SHGDNF flags) {
    STRRET strret{};
    if (FAILED(folder->GetDisplayNameOf(child, flags, &strret))) return {};
    wchar_t buffer[512]{};
    if (FAILED(StrRetToBufW(&strret, child, buffer, ARRAYSIZE(buffer)))) return {};
    return buffer;
}

struct Candidate {
    std::wstring parsing;   // identity: filesystem path or app id
    std::wstring name;      // label from the shell
    ParsingKind kind = ParsingKind::Unknown;
};

}  // namespace

std::vector<std::wstring> FindShortcuts(const std::wstring& root, int max_depth) {
    std::vector<std::wstring> found;
    if (root.empty() || max_depth < 0) return found;

    struct Frame {
        std::wstring dir;
        int depth;
    };
    std::vector<Frame> stack{{root, 0}};

    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();

        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((frame.dir + L"\\*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) continue;

        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = frame.dir + L"\\" + name;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (frame.depth + 1 <= max_depth) stack.push_back({full, frame.depth + 1});
                continue;
            }
            if (HasShortcutExtension(name)) found.push_back(full);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    std::sort(found.begin(), found.end());
    return found;
}

StartMenuScan ScanAppsFolder() {
    StartMenuScan scan;

    // AppsFolder is a virtual folder with no filesystem path, so it must be reached
    // through a PIDL. SHGetKnownFolderPath returns E_FAIL for it.
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &pidl);
    if (FAILED(hr)) {
        DF_WARN("shell", "SHGetKnownFolderIDList(AppsFolder) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        return scan;
    }

    ComPtr<IShellFolder> folder;
    hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
    CoTaskMemFree(pidl);
    if (FAILED(hr)) {
        DF_WARN("shell", "SHBindToObject(AppsFolder) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        return scan;
    }

    ComPtr<IEnumIDList> items;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &items);
    if (FAILED(hr)) {
        DF_WARN("shell", "IShellFolder::EnumObjects failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        return scan;
    }

    std::vector<Candidate> candidates;
    ITEMID_CHILD* child = nullptr;
    ULONG fetched = 0;
    while (items->Next(1, &child, &fetched) == S_OK && fetched == 1) {
        Candidate candidate;
        candidate.name = ChildName(folder.Get(), child, SHGDN_NORMAL);
        candidate.parsing = ChildName(folder.Get(), child, SHGDN_FORPARSING);
        candidate.kind = ClassifyParsingName(candidate.parsing);
        if (!candidate.parsing.empty()) candidates.push_back(std::move(candidate));
        CoTaskMemFree(child);
        child = nullptr;
    }

    // Deduplicate: the Start menu legitimately lists the same target twice (one per
    // language folder, or once per shortcut that points at the same exe).
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return _wcsicmp(a.parsing.c_str(), b.parsing.c_str()) < 0;
    });
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const Candidate& a, const Candidate& b) {
                                     return _wcsicmp(a.parsing.c_str(), b.parsing.c_str()) == 0;
                                 }),
                     candidates.end());

    for (const Candidate& candidate : candidates) {
        AppEntry entry;
        const ResolveError error = ResolveParsingName(candidate.parsing, candidate.name, entry);
        if (error != ResolveError::None) {
            ++scan.skipped;
            DF_DEBUG("shell", "skip %s (%s): %s", core::WideToUtf8(candidate.parsing).c_str(),
                     ToString(candidate.kind), ToString(error));
            continue;
        }
        if (!entry.launchable()) {
            ++scan.skipped;
            continue;
        }

        switch (entry.kind) {
            case AppKind::Uwp: ++scan.uwp_count; break;
            case AppKind::ShellItem: ++scan.desktop_id_count; break;
            case AppKind::NamespaceItem: ++scan.namespace_count; break;
            default: ++scan.shortcut_count; break;
        }
        scan.entries.push_back(std::move(entry));
    }

    std::sort(scan.entries.begin(), scan.entries.end(), [](const AppEntry& a, const AppEntry& b) {
        return _wcsicmp(a.display_name.c_str(), b.display_name.c_str()) < 0;
    });

    DF_INFO("shell",
            "AppsFolder scan: %zu entries (shortcut=%d uwp=%d desktop-id=%d namespace=%d "
            "skipped=%d)",
            scan.entries.size(), scan.shortcut_count, scan.uwp_count, scan.desktop_id_count,
            scan.namespace_count, scan.skipped);
    return scan;
}

std::vector<AppEntry> ListApps() { return ScanAppsFolder().entries; }

}  // namespace dockforge::shell
