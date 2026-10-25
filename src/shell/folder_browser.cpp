#include "shell/folder_browser.h"

#include <shlobj.h>
#include <shlwapi.h>

#include "core/log.h"
#include "core/paths.h"

#include <algorithm>
#include <cctype>

namespace dockforge::shell {
namespace {

constexpr char kLogModule[] = "folder";

// StrRet to std::wstring, freeing STRRET_WSTR ownership when present.
bool StrRetToWstr(const STRRET& strret, ITEMIDLIST_RELATIVE* pidl, std::wstring* out) {
    if (strret.uType == STRRET_WSTR && strret.pOleStr != nullptr) {
        *out = strret.pOleStr;
        CoTaskMemFree(strret.pOleStr);
        return true;
    }
    // STRRET_CSTR / STRRET_OFFSET still appear for some namespace items.
    wchar_t buf[MAX_PATH * 2] = {};
    if (SUCCEEDED(StrRetToBufW(const_cast<STRRET*>(&strret), pidl, buf, MAX_PATH * 2))) {
        *out = buf;
        return true;
    }
    return false;
}

}  // namespace

const char* ToString(FolderStatus status) {
    switch (status) {
        case FolderStatus::Ok: return "ok";
        case FolderStatus::NotFound: return "not-found";
        case FolderStatus::AccessDenied: return "access-denied";
        case FolderStatus::Failed: return "failed";
    }
    return "failed";
}

FolderStatus FolderStatusFromHresult(HRESULT hr) {
    // Only the failure families the panel can name are mapped; everything else
    // is "failed" rather than a guessed message (C-01 style honesty).
    if (SUCCEEDED(hr)) return FolderStatus::Ok;
    switch (hr) {
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
        case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND):
            return FolderStatus::NotFound;
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED):
            return FolderStatus::AccessDenied;
        default:
            return FolderStatus::Failed;
    }
}

FolderListing ListFolder(const std::wstring& dir) {
    FolderListing listing;
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);

    // Strip a trailing separator: SHParseDisplayName accepts both, but keeping
    // one canonical form makes drill-down paths and icon cache keys stable.
    // Drive roots ("C:\") are three chars and survive the trim by construction.
    std::wstring clean = dir;
    while (clean.size() > 3 && (clean.back() == L'\\' || clean.back() == L'/')) {
        clean.pop_back();
    }

    PIDLIST_ABSOLUTE pidl_dir = nullptr;
    HRESULT hr = SHParseDisplayName(clean.c_str(), nullptr, &pidl_dir, 0, nullptr);
    if (FAILED(hr)) {
        listing.status = FolderStatusFromHresult(hr);
        DF_WARN(kLogModule, "ListFolder(%s) parse failed: 0x%08lX (%s)",
                core::WideToUtf8(clean).c_str(), static_cast<unsigned long>(hr),
                ToString(listing.status));
        QueryPerformanceCounter(&t1);
        listing.elapsed_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                             static_cast<double>(freq.QuadPart);
        return listing;
    }

    IShellFolder* desktop = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&desktop))) {
        IShellFolder* folder = nullptr;
        hr = desktop->BindToObject(pidl_dir, nullptr, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr)) {
            IEnumIDList* enum_ids = nullptr;
            hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                                     &enum_ids);
            if (hr == S_OK && enum_ids != nullptr) {
                listing.status = FolderStatus::Ok;
                for (;;) {
                    ITEMIDLIST_RELATIVE* child = nullptr;
                    ULONG fetched = 0;
                    if (enum_ids->Next(1, &child, &fetched) != S_OK || fetched == 0) break;
                    FolderEntry entry;
                    STRRET strret{};
                    if (SUCCEEDED(folder->GetDisplayNameOf(child, SIGDN_NORMALDISPLAY,
                                                           &strret))) {
                        StrRetToWstr(strret, child, &entry.display_name);
                    }
                    if (SUCCEEDED(folder->GetDisplayNameOf(
                            child, SIGDN_DESKTOPABSOLUTEPARSING, &strret))) {
                        StrRetToWstr(strret, child, &entry.full_path);
                    }
                    SFGAOF attrs = SFGAO_FOLDER;
                    if (SUCCEEDED(folder->GetAttributesOf(
                            1, const_cast<PCUITEMID_CHILD_ARRAY>(&child), &attrs))) {
                        entry.is_folder = (attrs & SFGAO_FOLDER) != 0;
                    }
                    listing.entries.push_back(std::move(entry));
                    ILFree(child);
                }
                enum_ids->Release();
            } else if (hr == S_FALSE) {
                // An empty folder is a fine folder: the panel shows its
                // "nothing here" placeholder, not an error.
                listing.status = FolderStatus::Ok;
            } else {
                listing.status = FolderStatusFromHresult(hr);
            }
            folder->Release();
        } else {
            listing.status = FolderStatusFromHresult(hr);
        }
        desktop->Release();
    } else {
        hr = E_FAIL;
        listing.status = FolderStatus::Failed;
    }
    CoTaskMemFree(pidl_dir);

    SortFolderEntries(listing.entries);
    QueryPerformanceCounter(&t1);
    listing.elapsed_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                         static_cast<double>(freq.QuadPart);
    if (listing.status != FolderStatus::Ok) {
        DF_WARN(kLogModule, "ListFolder(%s) failed: 0x%08lX (%s)",
                core::WideToUtf8(clean).c_str(), static_cast<unsigned long>(hr),
                ToString(listing.status));
    }
    return listing;
}

namespace {

// Case-insensitive wide compare without CRT locale surprises: towlower per
// code unit is good enough for ordering; ties fall through to the raw path.
int WideNameCompare(const std::wstring& a, const std::wstring& b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const wint_t ca = towlower(a[i]);
        const wint_t cb = towlower(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

}  // namespace

void SortFolderEntries(std::vector<FolderEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const FolderEntry& a, const FolderEntry& b) {
                         if (a.is_folder != b.is_folder) return a.is_folder > b.is_folder;
                         const int by_name = WideNameCompare(a.display_name, b.display_name);
                         if (by_name != 0) return by_name < 0;
                         // Total order: two items with the same display name (a
                         // file and its " - Copy", or two virtual items) still
                         // get a deterministic slot from their parsing paths.
                         return a.full_path < b.full_path;
                     });
}

std::wstring PlaceholderFor(FolderStatus status) {
    switch (status) {
        case FolderStatus::Ok:
            return L"（空文件夹）";
        case FolderStatus::NotFound:
            return L"（文件夹不存在）";
        case FolderStatus::AccessDenied:
            return L"（无权限访问此文件夹）";
        case FolderStatus::Failed:
            return L"（无法读取此文件夹）";
    }
    return L"（无法读取此文件夹）";
}

void FolderNavStack::Push(std::wstring dir) {
    if (dir.empty()) return;
    stack_.push_back(std::move(dir));
}

bool FolderNavStack::Pop() {
    if (AtRoot()) return false;
    stack_.pop_back();
    return true;
}

}  // namespace dockforge::shell
