// DockForge T-066 spike: IShellFolder::EnumObjects + IEnumIDList under MinGW-w64.
//
// Question (TASKS.md DoD 1): can the folder drill-through browser enumerate a
// directory's display names, icons and PIDLs with plain MinGW (no MSVC, no
// C++/WinRT), and what happens on the failure paths the panel must survive
// (empty folder, access-denied folder, missing path)? Also: is SHCreateDataObject
// available for the CF_HDROP drag-out half of ADR-0023?
//
// Everything is measured against a scratch directory the spike creates itself,
// plus two system paths that exercise the error paths for free:
//   * C:\System Volume Information  - access denied for a normal user token
//   * a path that does not exist
//
// Build (Git Bash, repo root):
//   g++ -std=c++17 -O0 -g -o spikes/T-066_shellfolder_enum/spike.exe \
//       spikes/T-066_shellfolder_enum/spike.cpp -lole32 -lshell32 -lshlwapi -luser32 -lgdi32

#include <shlwapi.h>
#include <shlobj.h>
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failed = 0;

void Check(bool ok, const char* what, const std::string& detail = {}) {
    ++g_checks;
    std::printf("%s  %s%s%s\n", ok ? "[PASS]" : "[FAIL]", what,
                detail.empty() ? "" : "  ", detail.c_str());
    if (!ok) ++g_failed;
}

template <typename... Args>
std::string Fmt(const char* fmt, Args... args) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, args...);
    return buf;
}

std::string Narrow(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                          static_cast<int>(wide.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        out.data(), bytes, nullptr, nullptr);
    return out;
}

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

struct Child {
    std::wstring display;     // SIGDN_NORMALDISPLAY - what the panel draws
    std::wstring parsing;     // SIGDN_DESKTOPABSOLUTEPARSING - full path via PIDL
    std::wstring relative;    // SIGDN_PARENTRELATIVEPARSING
    bool is_folder = false;
    HICON icon = nullptr;     // SHGFI_PIDL direct HICON
    int imagelist_index = -1; // SHGFI_SYSICONINDEX + IImageList (higher DPI quality)
    UINT pidl_bytes = 0;      // absolute PIDL size, proves PIDL is real
};

// One enumeration pass over `dir`. Returns S_OK/S_FALSE or the failing HRESULT so
// the error paths can be logged verbatim.
HRESULT EnumerateOne(const std::wstring& dir, ULONG flags, std::vector<Child>* out,
                     double* elapsed_ms) {
    out->clear();
    LARGE_INTEGER t0{}, t1{}, freq{};
    QueryPerformanceCounter(&t0);
    QueryPerformanceFrequency(&freq);

    PIDLIST_ABSOLUTE pidl_dir = nullptr;
    HRESULT hr = SHParseDisplayName(dir.c_str(), nullptr, &pidl_dir, 0, nullptr);
    if (FAILED(hr)) {
        QueryPerformanceCounter(&t1);
        *elapsed_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                      static_cast<double>(freq.QuadPart);
        return hr;
    }

    IShellFolder* desktop = nullptr;
    hr = SHGetDesktopFolder(&desktop);
    if (SUCCEEDED(hr)) {
        IShellFolder* folder = nullptr;
        hr = desktop->BindToObject(pidl_dir, nullptr, IID_PPV_ARGS(&folder));
        if (SUCCEEDED(hr)) {
            IEnumIDList* enum_ids = nullptr;
            hr = folder->EnumObjects(nullptr, flags, &enum_ids);
            if (hr == S_OK && enum_ids != nullptr) {
                for (;;) {
                    ITEMIDLIST_RELATIVE* child = nullptr;
                    ULONG fetched = 0;
                    if (enum_ids->Next(1, &child, &fetched) != S_OK || fetched == 0) break;
                    Child entry;
                    STRRET strret{};
                    if (SUCCEEDED(folder->GetDisplayNameOf(child, SIGDN_NORMALDISPLAY,
                                                           &strret))) {
                        StrRetToWstr(strret, child, &entry.display);
                    }
                    if (SUCCEEDED(folder->GetDisplayNameOf(
                            child, SIGDN_DESKTOPABSOLUTEPARSING, &strret))) {
                        StrRetToWstr(strret, child, &entry.parsing);
                    }
                    if (SUCCEEDED(folder->GetDisplayNameOf(
                            child, SIGDN_PARENTRELATIVEPARSING, &strret))) {
                        StrRetToWstr(strret, child, &entry.relative);
                    }
                    SFGAOF attrs = SFGAO_FOLDER;
                    if (SUCCEEDED(folder->GetAttributesOf(
                            1, const_cast<PCUITEMID_CHILD_ARRAY>(&child), &attrs))) {
                        entry.is_folder = (attrs & SFGAO_FOLDER) != 0;
                    }
                    // Icon, direct HICON from the absolute PIDL.
                    SHFILEINFOW sfi{};
                    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl_dir), 0, &sfi,
                                       sizeof(sfi),
                                       SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON)) {
                        entry.icon = sfi.hIcon;
                    }
                    // Icon, system image-list index (what a D2D panel would use).
                    SHFILEINFOW sfi2{};
                    if (SHGetFileInfoW(reinterpret_cast<LPCWSTR>(pidl_dir), 0, &sfi2,
                                       sizeof(sfi2), SHGFI_PIDL | SHGFI_SYSICONINDEX)) {
                        entry.imagelist_index = static_cast<int>(sfi2.iIcon);
                    }
                    // Walk the absolute PIDL to its terminator for a byte count:
                    // proves the PIDL is a real multi-item list, not a stub.
                    UINT bytes = 0;
                    {
                        const ITEMIDLIST* walk = pidl_dir;
                        while (walk->mkid.cb != 0) {
                            bytes += walk->mkid.cb;
                            walk = reinterpret_cast<const ITEMIDLIST*>(
                                reinterpret_cast<const BYTE*>(walk) + walk->mkid.cb);
                        }
                        bytes += sizeof(USHORT);  // terminator
                    }
                    entry.pidl_bytes = bytes;
                    out->push_back(entry);
                    ILFree(child);
                }
                enum_ids->Release();
            }
            // hr == S_FALSE: no enumerator because the filter matched nothing
            // (empty folder) - distinct from an error.
            folder->Release();
        }
        desktop->Release();
    }
    CoTaskMemFree(pidl_dir);
    QueryPerformanceCounter(&t1);
    *elapsed_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 /
                  static_cast<double>(freq.QuadPart);
    return hr;
}

}  // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ---- scratch tree: subfolder + a .txt + a long-named file ----
    wchar_t temp_root[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp_root);
    const std::wstring dir = std::wstring(temp_root) + L"dockforge_t066_spike";
    const std::wstring subdir = dir + L"\\subdir";
    const std::wstring file = dir + L"\\hello.txt";
    const std::wstring longname =
        dir + L"\\a-very-long-file-name-that-must-be-truncated-by-the-panel-row.txt";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(subdir.c_str(), nullptr);
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = CreateFileW(longname.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);

    std::printf("== T-066 spike: IShellFolder::EnumObjects under MinGW ==\n");
    std::printf("scratch dir: %s\n\n", Narrow(dir).c_str());

    double ms = 0.0;
    std::vector<Child> children;
    HRESULT hr = EnumerateOne(dir, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &children, &ms);
    std::printf("scratch dir: hr=0x%08lX, %zu children, %.2f ms\n",
                static_cast<unsigned long>(hr), children.size(), ms);
    for (const Child& c : children) {
        std::printf("  display=\"%s\" rel=\"%s\" folder=%d icon=%p il=%d pidl=%uB\n",
                    Narrow(c.display).c_str(), Narrow(c.relative).c_str(),
                    c.is_folder ? 1 : 0, static_cast<void*>(c.icon), c.imagelist_index,
                    c.pidl_bytes);
        if (c.icon) DestroyIcon(c.icon);
    }
    Check(SUCCEEDED(hr), "EnumObjects on scratch dir",
          Fmt("hr=0x%08lX children=%zu %.2fms", static_cast<unsigned long>(hr),
              children.size(), ms));
    Check(children.size() == 3, "3 children enumerated",
          Fmt("count=%zu", children.size()));
    int folders = 0, files = 0, icons = 0, il_idx = 0, nonempty_pidl = 0;
    bool names_ok = true, parsing_ok = true;
    for (const Child& c : children) {
        folders += c.is_folder ? 1 : 0;
        files += c.is_folder ? 0 : 1;
        icons += c.icon != nullptr ? 1 : 0;
        il_idx += c.imagelist_index >= 0 ? 1 : 0;
        nonempty_pidl += c.pidl_bytes > 0 ? 1 : 0;
        if (c.display.empty()) names_ok = false;
        if (c.parsing.rfind(dir, 0) != 0) parsing_ok = false;
    }
    Check(folders == 1, "SFGAO_FOLDER on subdir", Fmt("folders=%d", folders));
    Check(files == 2, "2 non-folder children", Fmt("files=%d", files));
    Check(icons == 3, "HICON via SHGFI_PIDL for every child", Fmt("icons=%d", icons));
    Check(il_idx == 3, "image-list index via SHGFI_SYSICONINDEX for every child",
          Fmt("il=%d", il_idx));
    Check(nonempty_pidl == 3, "absolute PIDL non-empty for every child",
          Fmt("pidls=%d", nonempty_pidl));
    Check(names_ok, "SIGDN_NORMALDISPLAY non-empty for every child");
    Check(parsing_ok, "SIGDN_DESKTOPABSOLUTEPARSING gives full path under the dir");

    // ---- empty folder ----
    const std::wstring empty_dir = dir + L"\\emptydir";
    CreateDirectoryW(empty_dir.c_str(), nullptr);
    hr = EnumerateOne(empty_dir, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &children, &ms);
    std::printf("\nempty dir: hr=0x%08lX, %zu children\n", static_cast<unsigned long>(hr),
                children.size());
    Check(hr == S_OK && children.empty(),
          "empty folder: S_OK with zero children (not an error)");

    // ---- access denied ----
    hr = EnumerateOne(L"C:\\System Volume Information",
                      SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &children, &ms);
    std::printf("\naccess-denied dir: hr=0x%08lX, %zu children\n",
                static_cast<unsigned long>(hr), children.size());
    Check(FAILED(hr) || children.empty(),
          "access-denied folder fails or is empty - panel shows placeholder");

    // ---- nonexistent path ----
    hr = EnumerateOne(L"C:\\__dockforge_no_such_dir_9f2c__", SHCONTF_FOLDERS, &children,
                      &ms);
    std::printf("\nmissing dir: hr=0x%08lX\n", static_cast<unsigned long>(hr));
    Check(FAILED(hr), "missing path fails with an HRESULT the panel can map");

    // ---- drag-out feasibility: SHCreateDataObject -> CF_HDROP ----
    {
        PIDLIST_ABSOLUTE pidl_dir = nullptr;
        hr = SHParseDisplayName(dir.c_str(), nullptr, &pidl_dir, 0, nullptr);
        if (SUCCEEDED(hr)) {
            IDataObject* data = nullptr;
            HRESULT hr_data = E_FAIL;
            {
                IShellFolder* desktop = nullptr;
                if (SUCCEEDED(SHGetDesktopFolder(&desktop))) {
                    LPITEMIDLIST abs_child = nullptr;
                    if (SUCCEEDED(SHParseDisplayName(file.c_str(), nullptr, &abs_child, 0,
                                                     nullptr))) {
                        IShellFolder* parent = nullptr;
                        PCUITEMID_CHILD rel = nullptr;
                        if (SUCCEEDED(SHBindToParent(abs_child, IID_PPV_ARGS(&parent),
                                                     &rel))) {
                            ITEMIDLIST_RELATIVE* rel_copy = ILClone(rel);
                            hr_data = SHCreateDataObject(
                                pidl_dir, 1,
                                const_cast<PCUITEMID_CHILD_ARRAY>(&rel_copy), nullptr,
                                IID_PPV_ARGS(&data));
                            ILFree(rel_copy);
                            parent->Release();
                        }
                        ILFree(abs_child);
                    }
                    desktop->Release();
                }
            }
            std::printf("\n");
            Check(SUCCEEDED(hr_data) && data != nullptr,
                  "SHCreateDataObject(pidlFolder, 1 pidl) -> IDataObject",
                  Fmt("hr=0x%08lX", static_cast<unsigned long>(hr_data)));
            if (data != nullptr) {
                FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                HRESULT q = data->QueryGetData(&fmt);
                Check(q == S_OK, "QueryGetData(CF_HDROP) == S_OK (drag payload ready)",
                      Fmt("hr=0x%08lX", static_cast<unsigned long>(q)));
                data->Release();
            }
            CoTaskMemFree(pidl_dir);
        }
    }

    // ---- cleanup scratch ----
    DeleteFileW(file.c_str());
    DeleteFileW(longname.c_str());
    RemoveDirectoryW(subdir.c_str());
    RemoveDirectoryW(empty_dir.c_str());
    RemoveDirectoryW(dir.c_str());

    std::printf("\n== %d checks, %d failed ==\n", g_checks, g_failed);
    CoUninitialize();
    return g_failed == 0 ? 0 : 1;
}
