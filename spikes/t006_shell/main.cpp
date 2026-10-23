// spikes/t006_shell - prove the four shell APIs the Dock needs before they go
// into the main project.
//
//   spike_t006_shell --mode=icon   --path=C:\Windows\explorer.exe --size=256
//   spike_t006_shell --mode=lnk    --path=%APPDATA%\...\Something.lnk
//   spike_t006_shell --mode=uwp    --aumid=Microsoft.WindowsCalculator_8wekyb3d8bbwe!App
//   spike_t006_shell --mode=enum   [--dir=<start menu programs>]
//
// Each mode prints what it resolved and where it wrote any bitmap, so the report
// itself is the evidence. Nothing here is decorative: every call is one the Dock
// will make per icon, and the report records the HRESULT so a failure is a fact
// rather than a guess.
#include <windows.h>
#include <wincodec.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <propkey.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

// printf("%ls") silently truncates to empty for non-ASCII text under the default C
// locale, which makes a perfectly good localized name look like a failure. Every
// wide string that reaches the report goes through UTF-8 first.
std::string ToUtf8(const wchar_t* text) {
    if (!text) return {};
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring ArgValue(const std::vector<std::wstring>& args, const wchar_t* key) {
    const std::wstring prefix = std::wstring(key) + L"=";
    for (const auto& a : args) {
        if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    }
    return {};
}

bool HasFlag(const std::vector<std::wstring>& args, const wchar_t* flag) {
    for (const auto& a : args) {
        if (a == flag) return true;
    }
    return false;
}

// Writes an HBITMAP as a 32-bit PNG through WIC. This is the same path the icon// cache will use, so a failure here is a real failure, not a spike artefact.
HRESULT SavePng(HBITMAP bitmap, const std::wstring& path) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return hr;

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return hr;
    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return hr;
    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return hr;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return hr;
    hr = frame->Initialize(props.Get());
    if (FAILED(hr)) return hr;

    BITMAP bm{};
    if (!GetObjectW(bitmap, sizeof(bm), &bm)) return E_FAIL;
    hr = frame->SetSize(static_cast<UINT>(bm.bmWidth), static_cast<UINT>(bm.bmHeight));
    if (FAILED(hr)) return hr;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return hr;

    // HBITMAP from IShellItemImageFactory is top-down DIB data; WIC wants a
    // bottom-up or explicit-stride buffer, so read it through GetDIBits.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;  // negative = top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    const size_t stride = static_cast<size_t>(bm.bmWidth) * 4;
    std::vector<std::uint8_t> pixels(stride * static_cast<size_t>(bm.bmHeight));
    HDC screen = GetDC(nullptr);
    const int got = GetDIBits(screen, bitmap, 0, static_cast<UINT>(bm.bmHeight), pixels.data(),
                              &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    if (got == 0) return E_FAIL;

    hr = frame->WritePixels(static_cast<UINT>(bm.bmHeight), static_cast<UINT>(stride),
                            static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return hr;
    hr = frame->Commit();
    if (FAILED(hr)) return hr;
    return encoder->Commit();
}

void Report(const char* step, HRESULT hr) {
    std::printf("%-44s hr=0x%08lX %s\n", step, static_cast<unsigned long>(hr),
                SUCCEEDED(hr) ? "OK" : "FAIL");
    std::fflush(stdout);
}

int ModeIcon(const std::wstring& path, int size, const std::wstring& out) {
    const auto t0 = std::chrono::steady_clock::now();
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    Report("SHCreateItemFromParsingName", hr);
    if (FAILED(hr)) return 1;

    ComPtr<IShellItemImageFactory> factory;
    hr = item.As(&factory);
    Report("QueryInterface(IShellItemImageFactory)", hr);
    if (FAILED(hr)) return 1;

    SIZE want{size, size};
    HBITMAP bitmap = nullptr;
    // SIIGBF_BIGGERSIZEOK: accept a larger source and let us scale down, which is
    // what a Dock wants - a 256px icon must not come back as a blurry 32px one.
    // SIIGBF_ICONONLY: never hand back a shell thumbnail; a Dock shows icons.
    hr = factory->GetImage(want, static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY),
                           &bitmap);
    Report("GetImage(256, BIGGERSIZEOK|ICONONLY)", hr);
    if (FAILED(hr)) return 1;

    BITMAP bm{};
    GetObjectW(bitmap, sizeof(bm), &bm);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("bitmap=%dx%d bpp=%u cold_ms=%.2f\n", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel,
                ms);

    // Warm timing: the second call is served from the shell's own cache, which is
    // the number the Dock actually pays per icon after the first frame.
    const auto t2 = std::chrono::steady_clock::now();
    HBITMAP again = nullptr;
    hr = factory->GetImage(want, static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY),
                           &again);
    const auto t3 = std::chrono::steady_clock::now();
    const double warm = std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::printf("second GetImage warm_ms=%.2f hr=0x%08lX\n", warm,
                static_cast<unsigned long>(hr));
    if (again) DeleteObject(again);

    if (!out.empty()) {
        const HRESULT save = SavePng(bitmap, out);
        Report("SavePng", save);
    }
    DeleteObject(bitmap);
    return 0;
}

int ModeLnk(const std::wstring& path) {
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&link));
    Report("CoCreateInstance(CLSID_ShellLink)", hr);
    if (FAILED(hr)) return 1;

    ComPtr<IPersistFile> file;
    hr = link.As(&file);
    Report("QueryInterface(IPersistFile)", hr);
    if (FAILED(hr)) return 1;

    hr = file->Load(path.c_str(), STGM_READ);
    Report("IPersistFile::Load", hr);
    if (FAILED(hr)) return 1;

    wchar_t target[MAX_PATH * 2]{};
    WIN32_FIND_DATAW fd{};
    hr = link->GetPath(target, ARRAYSIZE(target), &fd, SLGP_RAWPATH);
    Report("GetPath(SLGP_RAWPATH)", hr);
    std::printf("target=%s\n", ToUtf8(target).c_str());

    wchar_t args[1024]{};
    hr = link->GetArguments(args, ARRAYSIZE(args));
    Report("GetArguments", hr);
    std::printf("arguments=%s\n", ToUtf8(args).c_str());

    wchar_t icon[MAX_PATH]{};
    int index = 0;
    hr = link->GetIconLocation(icon, ARRAYSIZE(icon), &index);
    Report("GetIconLocation", hr);
    std::printf("icon=%s index=%d\n", ToUtf8(icon).c_str(), index);

    wchar_t workdir[MAX_PATH]{};
    link->GetWorkingDirectory(workdir, ARRAYSIZE(workdir));
    std::printf("workdir=%s\n", ToUtf8(workdir).c_str());
    return 0;
}

int ModeUwp(const std::wstring& aumid) {
    if (aumid.empty()) {
        std::printf("--aumid is required for --mode=uwp\n");
        return 2;
    }
    ComPtr<IApplicationActivationManager> manager;
    // CLSCTX_LOCAL_SERVER is required: the manager lives in a separate process,
    // and CoCreateInstance with CLSCTX_INPROC_SERVER fails with REGDB_E_CLASSNOTREG.
    HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                 CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    Report("CoCreateInstance(ApplicationActivationManager)", hr);
    if (FAILED(hr)) return 1;

    // CoCreateInstance does NOT validate the AUMID: a made-up package family name
    // succeeds here too. Validation only happens when the shell namespace is asked
    // for the item, so the Dock must treat this as "the manager exists", not as
    // "this app exists".
    std::printf("aumid=%s accepted by manager (activation skipped on purpose)\n",
                ToUtf8(aumid.c_str()).c_str());

    // Resolve the icon the same way the Dock will: a UWP app's visual identity is
    // reachable through the shell namespace, and its AppUserModelID round-trips to
    // a shell item via the AppsFolder parsing name.
    const std::wstring parsing = L"shell:AppsFolder\\" + aumid;
    ComPtr<IShellItem> item;
    hr = SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item));
    Report("SHCreateItemFromParsingName(shell:AppsFolder\\AUMID)", hr);
    if (FAILED(hr)) return 1;

    // SIGDN_NORMALDISPLAY comes back EMPTY for AppsFolder items, so a Dock that
    // trusts it would show blank tooltips. The parsing-name forms are what carry
    // the app's identity; PARENTRELATIVEPARSING yields the bare AUMID.
    struct NameCase {
        SIGDN kind;
        const char* label;
    };
    const NameCase cases[] = {
        {SIGDN_NORMALDISPLAY, "SIGDN_NORMALDISPLAY"},
        {SIGDN_PARENTRELATIVEPARSING, "SIGDN_PARENTRELATIVEPARSING"},
        {SIGDN_DESKTOPABSOLUTEPARSING, "SIGDN_DESKTOPABSOLUTEPARSING"},
        {SIGDN_PARENTRELATIVEEDITING, "SIGDN_PARENTRELATIVEEDITING"},
    };
    for (const auto& c : cases) {
        PWSTR name = nullptr;
        const HRESULT nhr = item->GetDisplayName(c.kind, &name);
        std::printf("%-32s hr=0x%08lX name=%s\n", c.label,
                    static_cast<unsigned long>(nhr), ToUtf8(name).c_str());
        if (name) CoTaskMemFree(name);
    }

    // The display name lives in the property system, not in SIGDN_NORMALDISPLAY.
    // PKEY_ItemNameDisplay carries the resolved (localized) name; the AppUserModel
    // relaunch resource is an indirect "@dll,-id" string that SHLoadIndirectString
    // must expand. Both are recorded so the Dock's fallback order is evidence-based.
    ComPtr<IShellItem2> item2;
    if (SUCCEEDED(item.As(&item2))) {
        PWSTR display = nullptr;
        const HRESULT dhr = item2->GetString(PKEY_ItemNameDisplay, &display);
        std::printf("%-32s hr=0x%08lX name=%s\n", "PKEY_ItemNameDisplay",
                    static_cast<unsigned long>(dhr), ToUtf8(display).c_str());
        if (display) CoTaskMemFree(display);

        PWSTR relaunch = nullptr;
        const HRESULT rhr =
            item2->GetString(PKEY_AppUserModel_RelaunchDisplayNameResource, &relaunch);
        std::printf("%-32s hr=0x%08lX name=%s\n", "PKEY_AppUserModel_Relaunch...",
                    static_cast<unsigned long>(rhr), ToUtf8(relaunch).c_str());
        if (relaunch) {
            wchar_t expanded[512]{};
            const HRESULT xhr =
                SHLoadIndirectString(relaunch, expanded, ARRAYSIZE(expanded), nullptr);
            std::printf("%-32s hr=0x%08lX name=%s\n", "SHLoadIndirectString",
                        static_cast<unsigned long>(xhr),
                        SUCCEEDED(xhr) ? ToUtf8(expanded).c_str() : "(failed)");
            CoTaskMemFree(relaunch);
        }
    }

    ComPtr<IShellItemImageFactory> factory;
    if (SUCCEEDED(item.As(&factory))) {
        HBITMAP bitmap = nullptr;
        hr = factory->GetImage(SIZE{256, 256},
                               static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY),
                               &bitmap);
        Report("GetImage(uwp icon)", hr);
        if (SUCCEEDED(hr) && bitmap) {
            BITMAP bm{};
            GetObjectW(bitmap, sizeof(bm), &bm);
            std::printf("uwp icon=%dx%d\n", bm.bmWidth, bm.bmHeight);
            DeleteObject(bitmap);
        }
    }
    return 0;
}

// Proves whether IShellLinkW::GetPath output is usable as a path directly.
// Several real .lnk files store "%windir%\system32\control.exe"; without
// expansion, CreateProcess and SHCreateItemFromParsingName both fail.
int ModeExpand(const std::wstring& path) {
    ComPtr<IShellLinkW> link;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&link));
    if (FAILED(hr)) {
        Report("CoCreateInstance(CLSID_ShellLink)", hr);
        return 1;
    }
    ComPtr<IPersistFile> file;
    link.As(&file);
    hr = file->Load(path.c_str(), STGM_READ);
    if (FAILED(hr)) {
        Report("IPersistFile::Load", hr);
        return 1;
    }
    wchar_t raw[MAX_PATH * 2]{};
    WIN32_FIND_DATAW fd{};
    link->GetPath(raw, ARRAYSIZE(raw), &fd, SLGP_RAWPATH);
    std::printf("raw            =%s\n", ToUtf8(raw).c_str());

    wchar_t expanded[MAX_PATH * 2]{};
    const DWORD n = ExpandEnvironmentStringsW(raw, expanded, ARRAYSIZE(expanded));
    std::printf("expand_env_n   =%lu\n", n);
    std::printf("expanded       =%s\n", ToUtf8(expanded).c_str());
    std::printf("exists_raw     =%d\n", GetFileAttributesW(raw) != INVALID_FILE_ATTRIBUTES);
    std::printf("exists_expanded=%d\n", GetFileAttributesW(expanded) != INVALID_FILE_ATTRIBUTES);

    // Does the raw form work with the shell item API? If not, expansion is
    // mandatory before any icon lookup.
    ComPtr<IShellItem> item;
    const HRESULT raw_hr = SHCreateItemFromParsingName(raw, nullptr, IID_PPV_ARGS(&item));
    std::printf("SHCreateItem(raw)      hr=0x%08lX\n", static_cast<unsigned long>(raw_hr));
    item.Reset();
    const HRESULT exp_hr = SHCreateItemFromParsingName(expanded, nullptr, IID_PPV_ARGS(&item));
    std::printf("SHCreateItem(expanded) hr=0x%08lX\n", static_cast<unsigned long>(exp_hr));
    return 0;
}

int ModeEnum(const std::wstring& dir) {
    if (dir.empty()) {
        std::printf("--dir is required for --mode=enum\n");
        return 2;
    }
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) {
        std::printf("FindFirstFileW failed: %lu\n", GetLastError());
        return 1;
    }
    int lnk = 0;
    int exe = 0;
    int other = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring name = fd.cFileName;
        const size_t dot = name.find_last_of(L'.');
        const std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
        if (_wcsicmp(ext.c_str(), L".lnk") == 0) ++lnk;
        else if (_wcsicmp(ext.c_str(), L".exe") == 0) ++exe;
        else ++other;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    std::printf("dir=%s lnk=%d exe=%d other=%d\n", ToUtf8(dir.c_str()).c_str(), lnk, exe, other);
    return 0;
}

// Enumerates the shell's AppsFolder, which is what the Start menu itself reads.
// This is the only path that yields BOTH Win32 shortcuts and UWP apps with a
// human-readable display name, because SIGDN_NORMALDISPLAY and PKEY_ItemNameDisplay
// are both empty when a UWP app is resolved directly from its AUMID.
int ModeAppsFolder(int limit) {
    // FOLDERID_AppsFolder is a VIRTUAL folder: it has no filesystem path, so
    // SHGetKnownFolderPath returns E_FAIL (0x80004005) for it. The working route is
    // a PIDL plus SHBindToObject; SHCreateItemFromParsingName on a path cannot work
    // because there is no path to parse.
    PIDLIST_ABSOLUTE pidl = nullptr;
    HRESULT hr = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &pidl);
    Report("SHGetKnownFolderIDList(AppsFolder)", hr);
    if (FAILED(hr)) return 1;

    ComPtr<IShellFolder> folder;
    hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder));
    Report("SHBindToObject(AppsFolder -> IShellFolder)", hr);
    CoTaskMemFree(pidl);
    if (FAILED(hr)) return 1;

    ComPtr<IEnumIDList> enum_ids;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enum_ids);
    Report("IShellFolder::EnumObjects", hr);
    if (FAILED(hr)) return 1;

    int total = 0;
    int with_name = 0;
    int uwp = 0;
    ITEMID_CHILD* child = nullptr;
    ULONG fetched = 0;
    while (enum_ids->Next(1, &child, &fetched) == S_OK && fetched == 1) {
        STRRET strret{};
        // SHGDN_NORMAL + SHGDN_INFOLDER is the pair the Start menu uses; FORPARSING
        // would give the AUMID or a filesystem path instead of a label.
        const HRESULT nhr = folder->GetDisplayNameOf(child, SHGDN_NORMAL, &strret);
        wchar_t name[512]{};
        if (SUCCEEDED(nhr)) StrRetToBufW(&strret, child, name, ARRAYSIZE(name));
        STRRET parse_ret{};
        wchar_t parsing[512]{};
        if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &parse_ret))) {
            StrRetToBufW(&parse_ret, child, parsing, ARRAYSIZE(parsing));
        }
        const bool is_uwp = wcschr(parsing, L'!') != nullptr;
        if (is_uwp) ++uwp;
        if (name[0] != L'\0') ++with_name;
        if (total < limit) {
            // The shell returns some names as private-use glyphs from the icon font
            // (a 1-2 character string in U+E000..U+F8FF). Printing the codepoints is
            // the only way to tell "empty" apart from "an icon character".
            char codes[64]{};
            int written = 0;
            for (int i = 0; name[i] && written < 40; ++i) {
                written += std::snprintf(codes + written, sizeof(codes) - written,
                                         "%04X ", static_cast<unsigned>(name[i]));
            }
            std::printf("  [%s] namelen=%d codes=%s| name=%s parsing=%s\n",
                        is_uwp ? "uwp" : "lnk", static_cast<int>(wcslen(name)), codes,
                        ToUtf8(name).c_str(), ToUtf8(parsing).c_str());
        }
        ++total;
        CoTaskMemFree(child);
        child = nullptr;
    }
    std::printf("apps_total=%d with_name=%d uwp=%d\n", total, with_name, uwp);
    return 0;
}

int ModeParse(const std::wstring& name, int size, const std::wstring& out) {
    // AppsFolder hands out names like "{1AC14E77-...}\cmd.exe": a shell-namespace
    // GUID followed by a child. SHCreateItemFromParsingName rejects those, so the
    // question is whether SHParseDisplayName can still reach the item.
    PIDLIST_ABSOLUTE pidl = nullptr;
    SFGAOF attrs = 0;
    const HRESULT hr_parse = SHParseDisplayName(name.c_str(), nullptr, &pidl, 0, &attrs);
    std::printf("SHParseDisplayName  hr=0x%08lX pidl=%p attrs=0x%08lX\n",
                static_cast<unsigned long>(hr_parse), static_cast<void*>(pidl),
                static_cast<unsigned long>(attrs));
    if (FAILED(hr_parse) || !pidl) {
        if (pidl) CoTaskMemFree(pidl);
        return 1;
    }

    ComPtr<IShellItem> item;
    const HRESULT hr_item = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item));
    std::printf("SHCreateItemFromIDList hr=0x%08lX\n", static_cast<unsigned long>(hr_item));

    wchar_t display[MAX_PATH * 2]{};
    if (SUCCEEDED(hr_item)) {
        PWSTR name_out = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name_out)) && name_out) {
            std::printf("normal_display      =%s\n", ToUtf8(name_out).c_str());
            CoTaskMemFree(name_out);
        } else {
            std::printf("normal_display      =<empty>\n");
        }
        if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &name_out)) && name_out) {
            std::printf("abs_parsing         =%s\n", ToUtf8(name_out).c_str());
            CoTaskMemFree(name_out);
        }
    }

    ComPtr<IShellItemImageFactory> factory;
    if (SUCCEEDED(hr_item)) {
        const HRESULT hr_qi = item.As(&factory);
        std::printf("QueryInterface(ImageFactory) hr=0x%08lX\n", static_cast<unsigned long>(hr_qi));
        if (SUCCEEDED(hr_qi)) {
            SIZE want{size, size};
            HBITMAP bitmap = nullptr;
            const HRESULT hr_img = factory->GetImage(
                want, static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY), &bitmap);
            std::printf("GetImage            hr=0x%08lX\n", static_cast<unsigned long>(hr_img));
            if (SUCCEEDED(hr_img) && bitmap) {
                BITMAP bm{};
                GetObjectW(bitmap, sizeof(bm), &bm);
                std::printf("bitmap=%dx%d bpp=%u\n", bm.bmWidth, bm.bmHeight, bm.bmBitsPixel);
                if (!out.empty()) Report("SavePng", SavePng(bitmap, out));
                DeleteObject(bitmap);
            }
        }
    }

    CoTaskMemFree(pidl);
    (void)display;
    return 0;
}

int ModeParent(int size) {
    // 54 AppsFolder entries come back with a parsing name shaped
    // "{1AC14E77-...}\cmd.exe". SHCreateItemFromParsingName rejects it (tested:
    // 0x80070002) because the string is only meaningful *relative to* the
    // AppsFolder namespace, not to the desktop root. The question this mode
    // answers: can we still reach those items, and get their icons, by binding
    // them through their parent folder?
    PIDLIST_ABSOLUTE root_pidl = nullptr;
    HRESULT hr = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &root_pidl);
    Report("SHGetKnownFolderIDList(AppsFolder)", hr);
    if (FAILED(hr)) return 1;

    ComPtr<IShellFolder> folder;
    hr = SHBindToObject(nullptr, root_pidl, nullptr, IID_PPV_ARGS(&folder));
    Report("SHBindToObject", hr);
    if (FAILED(hr)) {
        CoTaskMemFree(root_pidl);
        return 1;
    }

    ComPtr<IEnumIDList> enum_ids;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enum_ids);
    Report("EnumObjects", hr);
    if (FAILED(hr)) {
        CoTaskMemFree(root_pidl);
        return 1;
    }

    int total = 0;
    int tried = 0;
    int item_ok = 0;
    int name_ok = 0;
    int icon_ok = 0;
    int icon_via_extract = 0;
    int printed = 0;
    ITEMID_CHILD* child = nullptr;
    ULONG fetched = 0;
    while (enum_ids->Next(1, &child, &fetched) == S_OK && fetched == 1) {
        ++total;
        STRRET parse_ret{};
        wchar_t parsing[512]{};
        if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &parse_ret))) {
            StrRetToBufW(&parse_ret, child, parsing, ARRAYSIZE(parsing));
        }
        // Only the namespace-relative shapes matter here: a GUID prefix means the
        // parsing name is not a filesystem path.
        const bool namespace_relative = parsing[0] == L'{' && wcschr(parsing, L'\\') != nullptr;
        if (!namespace_relative) {
            CoTaskMemFree(child);
            child = nullptr;
            continue;
        }
        ++tried;

        ComPtr<IShellItem> item;
        const HRESULT hr_item = SHCreateItemWithParent(root_pidl, folder.Get(), child,
                                                       IID_PPV_ARGS(&item));
        if (SUCCEEDED(hr_item)) ++item_ok;

        wchar_t label[512]{};
        if (SUCCEEDED(hr_item)) {
            PWSTR name_out = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name_out)) && name_out) {
                wcscpy_s(label, name_out);
                CoTaskMemFree(name_out);
                if (label[0] != L'\0') ++name_ok;
            }
        }

        HRESULT hr_img = E_FAIL;
        if (SUCCEEDED(hr_item)) {
            ComPtr<IShellItemImageFactory> factory;
            if (SUCCEEDED(item.As(&factory))) {
                SIZE want{size, size};
                HBITMAP bitmap = nullptr;
                hr_img = factory->GetImage(
                    want, static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY), &bitmap);
                if (SUCCEEDED(hr_img) && bitmap) ++icon_ok;
                if (bitmap) DeleteObject(bitmap);
            }
        }

        // Which SIGDN survives a namespace-relative item? Whichever does, it is the
        // only thing a Dock can persist to config, because a PIDL is binary.
        if (printed < 4 && SUCCEEDED(hr_item)) {
            struct Probe {
                SIGDN kind;
                const char* label;
            };
            const Probe probes[] = {
                {SIGDN_DESKTOPABSOLUTEPARSING, "abs_parsing"},
                {SIGDN_PARENTRELATIVEPARSING, "parent_rel_parsing"},
                {SIGDN_DESKTOPABSOLUTEEDITING, "abs_editing"},
                {SIGDN_FILESYSPATH, "filesys_path"},
                {SIGDN_URL, "url"},
            };
            for (const Probe& probe : probes) {
                PWSTR text = nullptr;
                const HRESULT hr_name = item->GetDisplayName(probe.kind, &text);
                std::printf("      %-18s hr=0x%08lX value=%s\n", probe.label,
                            static_cast<unsigned long>(hr_name),
                            text ? ToUtf8(text).c_str() : "<null>");
                if (text) CoTaskMemFree(text);
            }
        }

        // Fallback that never needs an IShellItem: ask the parent folder for the
        // item's icon object directly.
        ComPtr<IExtractIconW> extract;
        HRESULT hr_extract = E_FAIL;
        if (FAILED(hr_img)) {
            // GetUIObjectOf wants a PCUITEMID_CHILD_ARRAY (an array of const child
            // pointers), not a plain ITEMID_CHILD**.
            PCUITEMID_CHILD child_array[1] = {child};
            hr_extract = folder->GetUIObjectOf(nullptr, 1, child_array, IID_IExtractIconW,
                                               nullptr,
                                               reinterpret_cast<void**>(extract.GetAddressOf()));
            if (SUCCEEDED(hr_extract)) {
                wchar_t loc[MAX_PATH]{};
                int index = 0;
                UINT flags = 0;
                const HRESULT hr_loc = extract->GetIconLocation(0, loc, ARRAYSIZE(loc), &index,
                                                                &flags);
                if (SUCCEEDED(hr_loc) && loc[0] != L'\0') ++icon_via_extract;
                if (printed < 8) {
                    std::printf("  extract hr=0x%08lX loc=%s index=%d flags=0x%X\n",
                                static_cast<unsigned long>(hr_loc), ToUtf8(loc).c_str(), index,
                                flags);
                }
            }
        }

        if (printed < 8) {
            std::printf("  [%d] parsing=%s\n      item=0x%08lX label=%s image=0x%08lX extract=0x%08lX\n",
                        printed, ToUtf8(parsing).c_str(), static_cast<unsigned long>(hr_item),
                        ToUtf8(label).c_str(), static_cast<unsigned long>(hr_img),
                        static_cast<unsigned long>(hr_extract));
            ++printed;
        }

        CoTaskMemFree(child);
        child = nullptr;
    }

    std::printf("total=%d namespace_relative=%d item_ok=%d label_ok=%d image_ok=%d "
                "extract_loc_ok=%d\n",
                total, tried, item_ok, name_ok, icon_ok, icon_via_extract);
    CoTaskMemFree(root_pidl);
    return 0;
}

int ModeReparse(int size) {
    // The 54 namespace-relative AppsFolder entries resolve fine through
    // SHCreateItemWithParent (labels + icons OK), but a Dock has to persist the
    // identity as TEXT in config.json and re-resolve it after a restart. So the
    // question is: does IShellFolder::ParseDisplayName accept the very string that
    // SIGDN_DESKTOPABSOLUTEPARSING hands back?
    PIDLIST_ABSOLUTE root_pidl = nullptr;
    HRESULT hr = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &root_pidl);
    Report("SHGetKnownFolderIDList(AppsFolder)", hr);
    if (FAILED(hr)) return 1;

    ComPtr<IShellFolder> folder;
    hr = SHBindToObject(nullptr, root_pidl, nullptr, IID_PPV_ARGS(&folder));
    Report("SHBindToObject", hr);
    if (FAILED(hr)) {
        CoTaskMemFree(root_pidl);
        return 1;
    }

    ComPtr<IEnumIDList> enum_ids;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enum_ids);
    Report("EnumObjects", hr);
    if (FAILED(hr)) {
        CoTaskMemFree(root_pidl);
        return 1;
    }

    int parsed_ok = 0;
    int path_ok = 0;
    int menu_ok = 0;
    int tried = 0;
    ITEMID_CHILD* child = nullptr;
    ULONG fetched = 0;
    while (enum_ids->Next(1, &child, &fetched) == S_OK && fetched == 1) {
        STRRET parse_ret{};
        wchar_t parsing[512]{};
        if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &parse_ret))) {
            StrRetToBufW(&parse_ret, child, parsing, ARRAYSIZE(parsing));
        }
        if (!(parsing[0] == L'{' && wcschr(parsing, L'\\') != nullptr)) {
            CoTaskMemFree(child);
            child = nullptr;
            continue;
        }
        ++tried;

        PIDLIST_RELATIVE child_pidl = nullptr;
        SFGAOF attrs = 0;
        const HRESULT hr_parse =
            folder->ParseDisplayName(nullptr, nullptr, parsing, nullptr, &child_pidl, &attrs);
        if (SUCCEEDED(hr_parse) && child_pidl) ++parsed_ok;

        // Is the re-parsed PIDL the same item? Compare the label from the original
        // enumeration against the label of the round-tripped PIDL.
        wchar_t label_round[512]{};
        if (SUCCEEDED(hr_parse) && child_pidl) {
            ComPtr<IShellItem> item;
            const HRESULT hr_item = SHCreateItemWithParent(root_pidl, folder.Get(), child_pidl,
                                                           IID_PPV_ARGS(&item));
            if (SUCCEEDED(hr_item)) {
                PWSTR text = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &text)) && text) {
                    wcscpy_s(label_round, text);
                    CoTaskMemFree(text);
                }
            }
        }

        // Absolute PIDL -> filesystem path, the cheapest launch identity if it works.
        wchar_t fs_path[MAX_PATH * 2]{};
        bool got_path = false;
        if (SUCCEEDED(hr_parse) && child_pidl) {
            PIDLIST_ABSOLUTE absolute = ILCombine(root_pidl, child_pidl);
            if (absolute) {
                got_path = SHGetPathFromIDListW(absolute, fs_path) != FALSE;
                if (got_path) ++path_ok;
                CoTaskMemFree(absolute);
            }
        }

        // Non-intrusive proof that the item can be launched: a context menu with at
        // least one verb. Actually invoking a verb would open windows on the desktop.
        ComPtr<IContextMenu> menu;
        HRESULT hr_menu = E_FAIL;
        if (SUCCEEDED(hr_parse) && child_pidl) {
            PCUITEMID_CHILD child_array[1] = {child_pidl};
            hr_menu = folder->GetUIObjectOf(nullptr, 1, child_array, IID_IContextMenu, nullptr,
                                            reinterpret_cast<void**>(menu.GetAddressOf()));
            if (SUCCEEDED(hr_menu)) {
                HMENU hmenu = CreatePopupMenu();
                const HRESULT hr_query = menu->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_NORMAL);
                if (SUCCEEDED(hr_query) && hr_query > 0) ++menu_ok;
                DestroyMenu(hmenu);
            }
        }

        if (tried <= 5) {
            std::printf("  parsing=%s\n      parse=0x%08lX roundtrip_label=%s fs_path=%s "
                        "menu=0x%08lX\n",
                        ToUtf8(parsing).c_str(), static_cast<unsigned long>(hr_parse),
                        ToUtf8(label_round).c_str(), got_path ? ToUtf8(fs_path).c_str() : "<none>",
                        static_cast<unsigned long>(hr_menu));
        }

        if (child_pidl) CoTaskMemFree(child_pidl);
        CoTaskMemFree(child);
        child = nullptr;
    }

    std::printf("tried=%d parsed_ok=%d fs_path_ok=%d context_menu_ok=%d\n", tried, parsed_ok,
                path_ok, menu_ok);
    CoTaskMemFree(root_pidl);
    return 0;
}

std::vector<DWORD> CmdProcessIds() {
    std::vector<DWORD> ids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return ids;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"cmd.exe") == 0) ids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return ids;
}
int ModeLaunch(const std::wstring& aumid) {
    // Probe 1: can ShellExecuteEx reach a namespace item when the string carries the
    // shell:AppsFolder prefix? (The bare "{GUID}\cmd.exe" form failed with lasterr=2.)
    {
        const std::vector<DWORD> before = CmdProcessIds();
        const wchar_t* target =
            L"shell:AppsFolder\\{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\\cmd.exe";
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
        info.lpVerb = L"open";
        info.lpFile = target;
        info.lpParameters = L"/c exit 7";
        info.nShow = SW_HIDE;
        const BOOL ok = ShellExecuteExW(&info);
        const DWORD last = ok ? 0 : GetLastError();
        std::printf("ShellExecuteEx(%s) ok=%d lasterr=%lu\n", ToUtf8(target).c_str(),
                    ok ? 1 : 0, last);
        if (ok && info.hProcess) {
            WaitForSingleObject(info.hProcess, 15000);
            DWORD code = 0xFFFFFFFF;
            GetExitCodeProcess(info.hProcess, &code);
            std::printf("  child exit code=%lu (expect 7)\n", code);
            CloseHandle(info.hProcess);
        }
        // Do not leave a stray console behind if the parameters were ignored.
        for (DWORD id : CmdProcessIds()) {
            if (std::find(before.begin(), before.end(), id) != before.end()) continue;
            HANDLE proc = OpenProcess(PROCESS_TERMINATE, FALSE, id);
            if (proc) {
                TerminateProcess(proc, 0);
                CloseHandle(proc);
                std::printf("  cleaned up stray cmd pid=%lu\n", id);
            }
        }
    }

    // Probe 2: packaged app activation. IApplicationActivationManager needs
    // CLSCTX_LOCAL_SERVER; ActivateApplication is the only documented way to start
    // an AUMID and it reports the created process id.
    {
        const std::wstring id = aumid.empty() ? L"Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"
                                              : aumid;
        ComPtr<IApplicationActivationManager> manager;
        HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                      CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
        Report("CoCreateInstance(ApplicationActivationManager)", hr);
        if (SUCCEEDED(hr)) {
            DWORD pid_out = 0;
            const HRESULT hr_act =
                manager->ActivateApplication(id.c_str(), nullptr, AO_NONE, &pid_out);
            std::printf("ActivateApplication(%s) hr=0x%08lX pid=%lu\n", ToUtf8(id.c_str()).c_str(),
                        static_cast<unsigned long>(hr_act), pid_out);
            if (SUCCEEDED(hr_act) && pid_out != 0) {
                // Confirm the pid is real before claiming success.
                HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid_out);
                std::printf("  OpenProcess(%lu) handle=%p\n", pid_out, static_cast<void*>(proc));
                if (proc) {
                    wchar_t image[MAX_PATH]{};
                    DWORD size = ARRAYSIZE(image);
                    QueryFullProcessImageNameW(proc, 0, image, &size);
                    std::printf("  image=%s\n", ToUtf8(image).c_str());
                    CloseHandle(proc);
                }
                // Leave no window behind.
                HANDLE kill = OpenProcess(PROCESS_TERMINATE, FALSE, pid_out);
                if (kill) {
                    TerminateProcess(kill, 0);
                    CloseHandle(kill);
                    std::printf("  terminated probe process\n");
                }
            }
        }
    }
    // Probe 3: can ActivateApplication start a Win32 app that registered a bare
    // AUMID ("Anysphere.Cursor")? If yes, ShellItem entries need no IContextMenu.
    {
        const wchar_t* ids[] = {L"Anysphere.Cursor", L"Chrome"};
        for (const wchar_t* raw_id : ids) {
            ComPtr<IApplicationActivationManager> manager;
            HRESULT hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                                          CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
            if (FAILED(hr)) {
                std::printf("ActivateApplication(%s) cocreate=0x%08lX\n", ToUtf8(raw_id).c_str(),
                            static_cast<unsigned long>(hr));
                continue;
            }
            DWORD pid_out = 0;
            const HRESULT hr_act = manager->ActivateApplication(raw_id, nullptr, AO_NONE, &pid_out);
            std::printf("ActivateApplication(%s) hr=0x%08lX pid=%lu\n", ToUtf8(raw_id).c_str(),
                        static_cast<unsigned long>(hr_act), pid_out);
            if (SUCCEEDED(hr_act) && pid_out != 0) {
                HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                          FALSE, pid_out);
                if (proc) {
                    wchar_t image[MAX_PATH]{};
                    DWORD size = ARRAYSIZE(image);
                    QueryFullProcessImageNameW(proc, 0, image, &size);
                    std::printf("  image=%s\n", ToUtf8(image).c_str());
                    // Leave nothing running: this is a probe, not a launch.
                    TerminateProcess(proc, 0);
                    CloseHandle(proc);
                    std::printf("  terminated probe process\n");
                }
            }
        }
    }

    // Probe 4: the fallback for a bare desktop AUMID. ActivateApplication rejected it
    // (0x80070057), so the question is whether ShellExecuteEx accepts the same
    // shell:AppsFolder form that worked for namespace items.
    {
        const wchar_t* targets[] = {
            L"shell:AppsFolder\\Anysphere.Cursor",
            L"shell:AppsFolder\\Chrome",
        };
        for (const wchar_t* target : targets) {
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
            info.lpVerb = L"open";
            info.lpFile = target;
            info.nShow = SW_SHOWNORMAL;
            const BOOL ok = ShellExecuteExW(&info);
            const DWORD last = ok ? 0 : GetLastError();
            std::printf("ShellExecuteEx(%s) ok=%d lasterr=%lu hinstApp=%p\n",
                        ToUtf8(target).c_str(), ok ? 1 : 0, last,
                        static_cast<void*>(info.hInstApp));
            if (ok) {
                // A launched app becomes a foreground window; the probe only needs to
                // know it started, then it is closed again to leave the desktop clean.
                Sleep(1500);
                HWND fg = GetForegroundWindow();
                DWORD fg_pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &fg_pid);
                std::printf("  foreground pid=%lu\n", fg_pid);
                if (fg_pid != 0 && fg_pid != GetCurrentProcessId()) {
                    HANDLE proc = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                              FALSE, fg_pid);
                    if (proc) {
                        wchar_t image[MAX_PATH]{};
                        DWORD size = ARRAYSIZE(image);
                        QueryFullProcessImageNameW(proc, 0, image, &size);
                        std::printf("  image=%s\n", ToUtf8(image).c_str());
                        TerminateProcess(proc, 0);
                        CloseHandle(proc);
                        std::printf("  terminated probe process\n");
                    }
                }
            }
        }
    }

    return 0;
}

int ModeInvoke(int size) {
    // ShellExecuteEx cannot reach the namespace-relative AppsFolder entries
    // (measured: lasterr=2 for the bare form; the shell:AppsFolder prefix works but
    // needs a parse round-trip). The route Explorer itself uses is IContextMenu on
    // the parent folder. This probe proves that route launches a real process, using
    // cmd.exe as the target so the effect is measurable and reversible.
    (void)size;
    const std::vector<DWORD> before = CmdProcessIds();

    PIDLIST_ABSOLUTE root_pidl = nullptr;
    HRESULT hr = SHGetKnownFolderIDList(FOLDERID_AppsFolder, 0, nullptr, &root_pidl);
    Report("SHGetKnownFolderIDList(AppsFolder)", hr);
    if (FAILED(hr)) return 1;

    ComPtr<IShellFolder> folder;
    hr = SHBindToObject(nullptr, root_pidl, nullptr, IID_PPV_ARGS(&folder));
    Report("SHBindToObject", hr);
    if (FAILED(hr)) {
        CoTaskMemFree(root_pidl);
        return 1;
    }

    ComPtr<IEnumIDList> enum_ids;
    hr = folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &enum_ids);
    Report("EnumObjects", hr);
    if (FAILED(hr)) {
        CoTaskMemFree(root_pidl);
        return 1;
    }

    ITEMID_CHILD* target_child = nullptr;
    ITEMID_CHILD* child = nullptr;
    ULONG fetched = 0;
    while (enum_ids->Next(1, &child, &fetched) == S_OK && fetched == 1) {
        STRRET parse_ret{};
        wchar_t parsing[512]{};
        if (SUCCEEDED(folder->GetDisplayNameOf(child, SHGDN_FORPARSING, &parse_ret))) {
            StrRetToBufW(&parse_ret, child, parsing, ARRAYSIZE(parsing));
        }
        if (StrStrIW(parsing, L"\\cmd.exe") != nullptr) {
            target_child = child;
            std::printf("target parsing=%s\n", ToUtf8(parsing).c_str());
            break;
        }
        CoTaskMemFree(child);
        child = nullptr;
    }

    if (!target_child) {
        std::printf("cmd.exe entry not found in AppsFolder\n");
        CoTaskMemFree(root_pidl);
        return 1;
    }

    PCUITEMID_CHILD child_array[1] = {target_child};
    ComPtr<IContextMenu> menu;
    hr = folder->GetUIObjectOf(nullptr, 1, child_array, IID_IContextMenu, nullptr,
                               reinterpret_cast<void**>(menu.GetAddressOf()));
    Report("GetUIObjectOf(IContextMenu)", hr);
    if (FAILED(hr)) {
        CoTaskMemFree(target_child);
        CoTaskMemFree(root_pidl);
        return 1;
    }

    HMENU hmenu = CreatePopupMenu();
    const HRESULT hr_query = menu->QueryContextMenu(hmenu, 0, 1, 0x7FFF, CMF_NORMAL);
    std::printf("QueryContextMenu hr=0x%08lX items=%ld\n", static_cast<unsigned long>(hr_query),
                static_cast<long>(hr_query));
    const UINT default_item = GetMenuDefaultItem(hmenu, FALSE, GMDI_USEDISABLED);
    std::printf("GetMenuDefaultItem=%u\n", default_item);

    // Read the verb text so the invocation is explicit rather than "whatever the
    // default happens to be on this machine".
    wchar_t verb[128]{};
    if (default_item > 0) {
        menu->GetCommandString(default_item - 1, GCS_VERBW, nullptr,
                               reinterpret_cast<char*>(verb), ARRAYSIZE(verb));
        std::printf("default verb=%s\n", ToUtf8(verb).c_str());
    }

    CMINVOKECOMMANDINFOEX info{};
    info.cbSize = sizeof(info);
    info.fMask = CMIC_MASK_FLAG_NO_UI;
    info.hwnd = nullptr;
    // Invoking by menu offset is the form Explorer uses and avoids the
    // ANSI/Unicode verb-pointer ambiguity that returned E_INVALIDARG.
    if (default_item > 0) {
        info.lpVerb = MAKEINTRESOURCEA(default_item - 1);
    } else {
        info.lpVerb = "open";
    }
    info.nShow = SW_SHOWNORMAL;
    const HRESULT hr_invoke = menu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&info));
    std::printf("InvokeCommand hr=0x%08lX\n", static_cast<unsigned long>(hr_invoke));

    DestroyMenu(hmenu);

    Sleep(1200);
    const std::vector<DWORD> after = CmdProcessIds();
    std::printf("cmd.exe processes before=%zu after=%zu\n", before.size(), after.size());
    for (DWORD id : after) {
        const bool existed = std::find(before.begin(), before.end(), id) != before.end();
        if (existed) continue;
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, id);
        wchar_t image[MAX_PATH]{};
        DWORD size_out = ARRAYSIZE(image);
        if (proc) {
            QueryFullProcessImageNameW(proc, 0, image, &size_out);
            std::printf("  new pid=%lu image=%s\n", id, ToUtf8(image).c_str());
            TerminateProcess(proc, 0);
            CloseHandle(proc);
        }
    }

    CoTaskMemFree(target_child);
    CoTaskMemFree(root_pidl);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args(argv + 1, argv + argc);
    const std::wstring mode = ArgValue(args, L"--mode");
    const std::wstring path = ArgValue(args, L"--path");
    const std::wstring aumid = ArgValue(args, L"--aumid");
    const std::wstring dir = ArgValue(args, L"--dir");
    const int size = _wtoi(ArgValue(args, L"--size").c_str()) > 0
                         ? _wtoi(ArgValue(args, L"--size").c_str())
                         : 256;
    const std::wstring out = ArgValue(args, L"--out");
    const std::wstring limit_arg = ArgValue(args, L"--limit");
    const int limit = limit_arg.empty() ? 12 : _wtoi(limit_arg.c_str());

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("mode=%s com=0x%08lX\n", ToUtf8(mode.c_str()).c_str(), static_cast<unsigned long>(hr_com));

    int rc = 2;
    if (mode == L"icon") rc = ModeIcon(path, size, out);
    else if (mode == L"lnk") rc = ModeLnk(path);
    else if (mode == L"uwp") rc = ModeUwp(aumid);
    else if (mode == L"expand") rc = ModeExpand(path);
    else if (mode == L"enum") rc = ModeEnum(dir);
    else if (mode == L"appsfolder") rc = ModeAppsFolder(limit);
    else if (mode == L"parse") rc = ModeParse(path, size, out);
    else if (mode == L"parent") rc = ModeParent(size);
    else if (mode == L"reparse") rc = ModeReparse(size);
    else if (mode == L"launch") rc = ModeLaunch(aumid);
    else if (mode == L"invoke") rc = ModeInvoke(size);
    else std::printf("unknown --mode\n");

    CoUninitialize();
    return rc;
}
