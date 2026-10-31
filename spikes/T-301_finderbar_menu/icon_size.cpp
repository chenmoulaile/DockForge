// Dock icon quality probe: what does the shell actually hand back?
//
// The dock asks IShellItemImageFactory for a 256 px icon with
// SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY. BIGGERSIZEOK lets the shell return a
// *larger* bitmap than requested, which is fine, but it also lets it return the
// nearest available size - so a 48 px icon can come back and be stretched to
// 256 px, which is exactly what "worse than the native icon" looks like.
// This probe measures the real returned size for the dock's own tile list.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>

static FILE* g_log = nullptr;
static void L(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}

static void Probe(const wchar_t* source, int want, SIIGBF flags) {
    IShellItem* item = nullptr;
    HRESULT hr = SHCreateItemFromParsingName(source, nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || !item) {
        L("  [%d %s] SHCreateItemFromParsingName=0x%08lX", want,
          flags == (SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY) ? "BIG" : "FIT",
          static_cast<unsigned long>(hr));
        return;
    }
    IShellItemImageFactory* f = nullptr;
    hr = item->QueryInterface(IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr) && f) {
        HBITMAP bmp = nullptr;
        hr = f->GetImage(SIZE{want, want}, flags, &bmp);
        if (SUCCEEDED(hr) && bmp) {
            BITMAP bm{};
            GetObjectW(bmp, sizeof(bm), &bm);
            L("  [%d %s] -> %ldx%ld bpp=%u", want,
              flags == (SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY) ? "BIG" : "FIT", bm.bmWidth,
              bm.bmHeight, bm.bmBitsPixel);
            DeleteObject(bmp);
        } else {
            L("  [%d %s] GetImage=0x%08lX", want,
              flags == (SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY) ? "BIG" : "FIT",
              static_cast<unsigned long>(hr));
        }
        f->Release();
    }
    item->Release();
}

int wmain(int argc, wchar_t** argv) {
    g_log = _wfopen(L"icon_size.log", L"w");
    if (!g_log) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::vector<std::wstring> targets;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i) targets.push_back(argv[i]);
    } else {
        wchar_t win[MAX_PATH]{};
        GetWindowsDirectoryW(win, MAX_PATH);
        targets.push_back(std::wstring(win) + L"\\System32\\notepad.exe");
        targets.push_back(std::wstring(win) + L"\\explorer.exe");
        targets.push_back(L"D:\\Sublime Text\\sublime_text.exe");
    }
    for (const auto& t : targets) {
        L("%ls", t.c_str());
        for (int want : {256, 192, 128, 96, 64, 48, 32}) {
            Probe(t.c_str(), want, static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY));
        }
        for (int want : {256, 192}) {
            Probe(t.c_str(), want, static_cast<SIIGBF>(SIIGBF_RESIZETOFIT | SIIGBF_ICONONLY));
        }
    }
    L("verdict: done");
    CoUninitialize();
    fclose(g_log);
    return 0;
}