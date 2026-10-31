// How much of the extracted square does each real icon actually occupy?
//
// icon_quality.cpp found notepad's artwork covers 58.4% of its 256px canvas and explorer's
// 81.5%, with the bbox offset toward the top. The dock stretches that whole square into a
// 48 dip slot, so a padded icon renders smaller than the slot it was sized for - the
// "smaller than the native Windows icon" complaint. Before choosing a normalisation rule,
// this measures the distribution across the icons the dock actually shows, because a rule
// that is right for notepad can be wrong for a document icon that is meant to look small.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
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

struct Box {
    bool ok = false;
    int minx = 0, miny = 0, maxx = 0, maxy = 0, size = 0;
    double coverage = 0.0;
    double fill = 0.0;  // longest bbox edge / canvas edge
};

static Box Measure(const wchar_t* source, int size) {
    Box box;
    box.size = size;
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(source, nullptr, IID_PPV_ARGS(&item))) || !item) return box;
    IShellItemImageFactory* f = nullptr;
    bool got = false;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&f))) && f) {
        HBITMAP bmp = nullptr;
        if (SUCCEEDED(f->GetImage(SIZE{size, size},
                                  static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY),
                                  &bmp)) && bmp) {
            BITMAP bm{};
            GetObjectW(bmp, sizeof(bm), &bm);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = bm.bmWidth;
            bi.bmiHeader.biHeight = -bm.bmHeight;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            std::vector<unsigned char> px((size_t)bm.bmWidth * bm.bmHeight * 4, 0);
            HDC dc = GetDC(nullptr);
            const int rows = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            if (rows == bm.bmHeight) {
                int minx = bm.bmWidth, miny = bm.bmHeight, maxx = -1, maxy = -1;
                long long opaque = 0;
                for (int y = 0; y < bm.bmHeight; ++y) {
                    for (int x = 0; x < bm.bmWidth; ++x) {
                        const int a = px[((size_t)y * bm.bmWidth + x) * 4 + 3];
                        if (a < 8) continue;
                        ++opaque;
                        if (x < minx) minx = x;
                        if (x > maxx) maxx = x;
                        if (y < miny) miny = y;
                        if (y > maxy) maxy = y;
                    }
                }
                if (maxx >= 0) {
                    box.ok = true;
                    box.minx = minx; box.miny = miny; box.maxx = maxx; box.maxy = maxy;
                    box.coverage = (double)opaque / ((double)bm.bmWidth * bm.bmHeight);
                    const int w = maxx - minx + 1, h = maxy - miny + 1;
                    box.fill = (double)(w > h ? w : h) / (double)bm.bmWidth;
                }
            }
            DeleteObject(bmp);
            got = true;
        }
        f->Release();
    }
    item->Release();
    (void)got;
    return box;
}

static void Report(const std::wstring& source) {
    const Box b = Measure(source.c_str(), 256);
    const std::wstring name = source.substr(source.find_last_of(L'\\') + 1);
    if (!b.ok) {
        L("%-28ls  no icon", name.c_str());
        return;
    }
    L("%-28ls bbox=%3d,%3d..%3d,%3d  fill=%5.1f%%  coverage=%5.1f%%  offset=%+d,%+d",
      name.c_str(), b.minx, b.miny, b.maxx, b.maxy, b.fill * 100.0, b.coverage * 100.0,
      b.minx - (b.size - (b.maxx - b.minx + 1)) / 2, b.miny - (b.size - (b.maxy - b.miny + 1)) / 2);
}

int wmain() {
    g_log = _wfopen(L"icon_fill.log", L"w");
    if (!g_log) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring sys = win;
    std::vector<std::wstring> targets = {
        sys + L"\\explorer.exe",
        sys + L"\\System32\\notepad.exe",
        sys + L"\\System32\\mspaint.exe",
        sys + L"\\System32\\calc.exe",
        sys + L"\\System32\\cmd.exe",
        sys + L"\\System32\\SnippingTool.exe",
        sys + L"\\System32\\Taskmgr.exe",
        sys + L"\\System32\\control.exe",
        sys + L"\\System32\\shell32.dll",
        sys + L"\\System32\\imageres.dll",
        sys + L"\\System32\\write.exe",
        sys + L"\\System32\\charmap.exe",
        sys + L"\\System32\\dxdiag.exe",
        sys + L"\\System32\\magnify.exe",
        sys + L"\\System32\\osk.exe",
        sys + L"\\System32\\perfmon.exe",
        sys + L"\\System32\\regedit.exe",
        sys + L"\\System32\\slui.exe",
        sys + L"\\System32\\winver.exe",
        sys + L"\\System32\\wmplayer.exe",
        sys + L"\\System32\\fsquirt.exe",
        sys + L"\\System32\\colorcpl.exe",
    };
    L("=== 256px canvas, alpha>=8 bbox");
    for (const auto& t : targets) Report(t);

    // Non-exe shell items: a folder and a text file, because the dock can pin both and
    // their artwork is intentionally smaller than an app icon.
    wchar_t profile[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile))) {
        L("=== shell items");
        Report(profile);
        Report(std::wstring(profile) + L"\\Desktop");
        Report(std::wstring(profile) + L"\\Documents");
        Report(std::wstring(sys) + L"\\System32\\drivers\\etc\\hosts");
    }
    L("verdict: done");
    CoUninitialize();
    fclose(g_log);
    return 0;
}