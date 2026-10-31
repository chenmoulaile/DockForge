// Decisive alpha-layout check for IShellItemImageFactory output.
//
// Q27 counted 6215/9427 partial-alpha texels whose colour exceeds alpha, which is
// impossible for premultiplied data. This dumps actual texels so the conclusion does
// not rest on an aggregate, and it cross-checks with a synthetic bitmap of known
// layout pushed through the same GetDIBits path.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shobjidl.h>
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

static bool Grab(const wchar_t* src, int size, std::vector<unsigned char>& px, int& w, int& h) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(src, nullptr, IID_PPV_ARGS(&item))) || !item) return false;
    IShellItemImageFactory* f = nullptr;
    bool ok = false;
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
            w = bm.bmWidth; h = bm.bmHeight;
            px.assign(static_cast<size_t>(w) * h * 4, 0);
            HDC dc = GetDC(nullptr);
            ok = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, px.data(), &bi, DIB_RGB_COLORS) == bm.bmHeight;
            ReleaseDC(nullptr, dc);
            DeleteObject(bmp);
        }
        f->Release();
    }
    item->Release();
    return ok;
}

int wmain() {
    g_log = _wfopen(L"alpha_check.log", L"w");
    if (!g_log) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring notepad = std::wstring(win) + L"\\System32\\notepad.exe";

    std::vector<unsigned char> px;
    int w = 0, h = 0;
    if (Grab(notepad.c_str(), 256, px, w, h)) {
        L("notepad 256x256 grabbed, %zux%zu bytes", (size_t)w, (size_t)h, px.size());
        // Print the first 12 partial-alpha texels with their raw values.
        int shown = 0;
        for (int i = 0; i < w * h && shown < 12; ++i) {
            const unsigned char* p = &px[(size_t)i * 4];
            const int b = p[0], g = p[1], r = p[2], a = p[3];
            if (a == 0 || a == 255) continue;
            L("  px[%d] B=%3d G=%3d R=%3d A=%3d  rgb<=a? %d", i, b, g, r, a,
              (b <= a && g <= a && r <= a) ? 1 : 0);
            ++shown;
        }
        // Histogram of how alpha relates to colour.
        long long a0 = 0, a255 = 0, apart = 0, viol = 0;
        for (int i = 0; i < w * h; ++i) {
            const unsigned char* p = &px[(size_t)i * 4];
            const int a = p[3];
            if (a == 0) { ++a0; continue; }
            if (a == 255) { ++a255; continue; }
            ++apart;
            if (p[0] > a || p[1] > a || p[2] > a) ++viol;
        }
        L("alpha=0:%lld alpha=255:%lld partial:%lld partial_violating:%lld", a0, a255, apart, viol);
    } else {
        L("grab failed");
    }

    // Control: a synthetic straight-alpha DIB of known content, pushed through the
    // same GetDIBits call, must come back unchanged. This proves the copy path does
    // not rewrite colour.
    {
        const int S = 4;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = S;
        bi.bmiHeader.biHeight = -S;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        auto* dst = static_cast<unsigned char*>(bits);
        // Straight alpha: full red at 25% alpha.
        for (int i = 0; i < S * S; ++i) {
            dst[i * 4 + 0] = 0;
            dst[i * 4 + 1] = 0;
            dst[i * 4 + 2] = 255;
            dst[i * 4 + 3] = 64;
        }
        std::vector<unsigned char> out((size_t)S * S * 4, 0);
        const int rows = GetDIBits(screen, dib, 0, S, out.data(), &bi, DIB_RGB_COLORS);
        L("control rows=%d first B=%d G=%d R=%d A=%d (expect 0,0,255,64)",
          rows, out[0], out[1], out[2], out[3]);
        DeleteObject(dib);
        ReleaseDC(nullptr, screen);
    }
    L("verdict: done");
    CoUninitialize();
    fclose(g_log);
    return 0;
}