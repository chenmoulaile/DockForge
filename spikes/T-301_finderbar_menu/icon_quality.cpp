// Dock icon quality probe.
//
// The complaint is that dock icons look worse than the native Windows icon. There are
// exactly three ways that can happen, so each one is measured instead of guessed:
//   Q27) alpha layout: is the shell's 32bpp bitmap premultiplied or straight? Tagging
//        a straight-alpha bitmap as D2D1_ALPHA_MODE_PREMULTIPLIED gives bright halos
//        on every anti-aliased edge.
//   Q28) detail loss: how much contrast survives 256 -> 96 with plain bilinear versus
//        a box filter? A 2.67x downscale by bilinear only samples 4 texels out of ~7,
//        which is where "mushy" comes from.
//   Q29) how much of the 256px square is actually opaque? A shell icon with 12% padding
//        drawn into a 48 dip slot is 12% smaller than the slot it was sized for.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <shobjidl.h>
#include <cstdio>
#include <cmath>
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

struct Img { int w = 0, h = 0; std::vector<unsigned char> px; };

static bool Grab(const wchar_t* source, int size, Img& out) {
    IShellItem* item = nullptr;
    if (FAILED(SHCreateItemFromParsingName(source, nullptr, IID_PPV_ARGS(&item))) || !item)
        return false;
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
            bi.bmiHeader.biHeight = -bm.bmHeight;  // top-down
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            out.w = bm.bmWidth;
            out.h = bm.bmHeight;
            out.px.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
            HDC dc = GetDC(nullptr);
            const int rows = GetDIBits(dc, bmp, 0, static_cast<UINT>(bm.bmHeight), out.px.data(),
                                       &bi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, dc);
            ok = rows == bm.bmHeight;
            DeleteObject(bmp);
        }
        f->Release();
    }
    item->Release();
    return ok;
}

// Q27: premultiplied means every colour channel is <= alpha.
static void Q27(const Img& img) {
    long long violations = 0, sampled = 0;
    int worst = 0;
    for (int i = 0; i < img.w * img.h; ++i) {
        const unsigned char* p = &img.px[static_cast<size_t>(i) * 4];
        const int a = p[3];
        if (a == 0 || a == 255) continue;  // edges of the alpha ramp are the interesting part
        ++sampled;
        for (int c = 0; c < 3; ++c) {
            if (p[c] > a) {
                ++violations;
                if (p[c] - a > worst) worst = p[c] - a;
                break;
            }
        }
    }
    L("[Q27] partial-alpha texels=%lld violations(rgb>a)=%lld worst_excess=%d", sampled,
      violations, worst);
    L("[Q27] => %s", violations == 0 ? "PREMULTIPLIED (current tag is correct)"
                                     : "STRAIGHT ALPHA (current PREMULTIPLIED tag is WRONG)");
}

// Q28: bilinear (2x2) vs box (all texels) when shrinking by an integer-ish factor.
static double DetailScore(const Img& src, int dst) {
    // Sum of |neighbour difference| over the destination: higher means crisper edges.
    const double scale = static_cast<double>(src.w) / dst;
    std::vector<double> lum(static_cast<size_t>(dst) * dst, 0.0);
    for (int y = 0; y < dst; ++y) {
        for (int x = 0; x < dst; ++x) {
            const double fx = x * scale, fy = y * scale;
            const int sx = static_cast<int>(fx), sy = static_cast<int>(fy);
            const auto at = [&](int px, int py) {
                px = px < 0 ? 0 : (px >= src.w ? src.w - 1 : px);
                py = py < 0 ? 0 : (py >= src.h ? src.h - 1 : py);
                const unsigned char* p = &src.px[(static_cast<size_t>(py) * src.w + px) * 4];
                return (0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2]);
            };
            lum[static_cast<size_t>(y) * dst + x] = at(sx, sy);
        }
    }
    double sum = 0.0;
    for (int y = 1; y < dst; ++y)
        for (int x = 1; x < dst; ++x) {
            const double c = lum[static_cast<size_t>(y) * dst + x];
            sum += std::fabs(c - lum[static_cast<size_t>(y - 1) * dst + x]);
            sum += std::fabs(c - lum[static_cast<size_t>(y) * dst + x - 1]);
        }
    return sum / (dst * dst);
}

// Q29: opaque bounding box.
static void Q29(const Img& img) {
    int minx = img.w, miny = img.h, maxx = -1, maxy = -1;
    long long opaque = 0;
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x) {
            const int a = img.px[(static_cast<size_t>(y) * img.w + x) * 4 + 3];
            if (a < 8) continue;
            ++opaque;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    if (maxx < 0) { L("[Q29] fully transparent"); return; }
    const double frac = static_cast<double>(opaque) / (img.w * img.h);
    L("[Q29] alpha>=8 bbox=%d,%d..%d,%d (%dx%d of %dx%d) coverage=%.1f%%", minx, miny, maxx, maxy,
      maxx - minx + 1, maxy - miny + 1, img.w, img.h, frac * 100.0);
}

int wmain(int argc, wchar_t** argv) {
    g_log = _wfopen(L"icon_quality.log", L"w");
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
    }
    for (const auto& t : targets) {
        L("=== %ls", t.c_str());
        Img big;
        if (!Grab(t.c_str(), 256, big)) { L("  grab failed"); continue; }
        L("[Q26] grabbed %dx%d", big.w, big.h);
        Q27(big);
        Q29(big);
        Img small;
        if (Grab(t.c_str(), 96, small)) {
            const double direct = DetailScore(small, 48);
            const double viaBig = DetailScore(big, 48);
            L("[Q28] detail@48 direct96=%.3f via256=%.3f ratio=%.2f", direct, viaBig,
              viaBig > 0.0 ? direct / viaBig : 0.0);
        }
    }
    L("verdict: done");
    CoUninitialize();
    fclose(g_log);
    return 0;
}