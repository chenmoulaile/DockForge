// Which downscale filter should the dock actually use?
//
// interp_check.cpp measured a mean-gradient "detail" score and found LINEAR ahead of
// HIGH_QUALITY_CUBIC. Gradient is a proxy, and a proxy can reward aliasing: bilinear
// sampling at a 2.67x reduction keeps high-frequency energy by folding it, which looks
// sharp in a gradient sum and looks like noise to a human. So this spike measures two
// things a proxy cannot fake:
//
//   alias  A 1px black/white checkerboard reduced 256 -> 96 must become uniform mid-grey.
//          Any residual variance is aliasing, full stop - there is no legitimate detail
//          left in a checkerboard at that reduction.
//   rms    Distance from the shell's own 96px rendition, and from a CPU box filter of the
//          same 256px source. Lower means the filter reproduces what the icon is supposed
//          to look like instead of inventing something.
//
// Both are measured on the same GPU path the app uses (D3D11 texture target +
// ID2D1DeviceContext::DrawBitmap), so the winner is the filter that renders best, not the
// filter that scores best on paper.
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

static FILE* g_log = nullptr;
static void L(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    va_end(ap);
    fflush(g_log);
}

struct Img {
    int w = 0, h = 0;
    std::vector<unsigned char> px;  // BGRA premultiplied, top-down
};

static void Premultiply(Img& img) {
    bool straight = false;
    for (size_t i = 0; i + 3 < img.px.size(); i += 4) {
        const unsigned a = img.px[i + 3];
        if (a != 0 && a != 255 && (img.px[i] > a || img.px[i + 1] > a || img.px[i + 2] > a)) {
            straight = true;
            break;
        }
    }
    if (!straight) return;
    for (size_t i = 0; i + 3 < img.px.size(); i += 4) {
        const unsigned a = img.px[i + 3];
        if (a == 0) {
            img.px[i] = img.px[i + 1] = img.px[i + 2] = 0;
            continue;
        }
        if (a == 255) continue;
        for (int c = 0; c < 3; ++c) img.px[i + c] = (unsigned char)((img.px[i + c] * a + 127u) / 255u);
    }
}

static bool GrabIcon(const wchar_t* source, int size, Img& out) {
    ComPtr<IShellItem> item;
    if (FAILED(SHCreateItemFromParsingName(source, nullptr, IID_PPV_ARGS(&item)))) return false;
    ComPtr<IShellItemImageFactory> factory;
    if (FAILED(item.As(&factory))) return false;
    HBITMAP bmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{size, size},
                                 static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY),
                                 &bmp)) || !bmp) {
        return false;
    }
    BITMAP bm{};
    GetObjectW(bmp, sizeof(bm), &bm);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.w = bm.bmWidth;
    out.h = bm.bmHeight;
    out.px.assign((size_t)out.w * out.h * 4, 0);
    HDC dc = GetDC(nullptr);
    const int rows = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, out.px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(bmp);
    if (rows != bm.bmHeight) return false;
    Premultiply(out);
    return true;
}

// Correct box filter for premultiplied alpha: average the premultiplied values. Averaging
// straight-alpha colour would weight fully transparent texels as if they were opaque.
static Img BoxDownscale(const Img& src, int dst) {
    Img out;
    out.w = dst;
    out.h = dst;
    out.px.assign((size_t)dst * dst * 4, 0);
    const double scale = (double)src.w / dst;
    for (int y = 0; y < dst; ++y) {
        for (int x = 0; x < dst; ++x) {
            const double x0 = x * scale, x1 = (x + 1) * scale;
            const double y0 = y * scale, y1 = (y + 1) * scale;
            const int ix0 = (int)std::floor(x0), ix1 = (int)std::ceil(x1);
            const int iy0 = (int)std::floor(y0), iy1 = (int)std::ceil(y1);
            double acc[4] = {0, 0, 0, 0};
            double weight = 0.0;
            for (int sy = iy0; sy < iy1; ++sy) {
                if (sy < 0 || sy >= src.h) continue;
                const double wy = std::min(y1, (double)sy + 1) - std::max(y0, (double)sy);
                if (wy <= 0) continue;
                for (int sx = ix0; sx < ix1; ++sx) {
                    if (sx < 0 || sx >= src.w) continue;
                    const double wx = std::min(x1, (double)sx + 1) - std::max(x0, (double)sx);
                    if (wx <= 0) continue;
                    const double w = wx * wy;
                    const unsigned char* p = &src.px[((size_t)sy * src.w + sx) * 4];
                    for (int c = 0; c < 4; ++c) acc[c] += p[c] * w;
                    weight += w;
                }
            }
            unsigned char* o = &out.px[((size_t)y * dst + x) * 4];
            for (int c = 0; c < 4; ++c) {
                o[c] = weight > 0 ? (unsigned char)std::lround(acc[c] / weight) : 0;
            }
        }
    }
    return out;
}

static double Rms(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    double sum = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        const double d = (double)a[i] - (double)b[i];
        sum += d * d;
    }
    return std::sqrt(sum / (double)a.size());
}

static std::vector<unsigned char> RenderMode(ID2D1DeviceContext* dc, ID2D1Bitmap* src_bitmap,
                                            ID3D11Device* d3d, ID3D11DeviceContext* d3d_ctx, int dst,
                                            D2D1_INTERPOLATION_MODE mode) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = dst;
    desc.Height = dst;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &texture))) return {};
    ComPtr<IDXGISurface> surface;
    if (FAILED(texture.As(&surface))) return {};
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &target))) return {};
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    dc->DrawBitmap(src_bitmap, D2D1::RectF(0, 0, (float)dst, (float)dst), 1.0f, mode, nullptr,
                   nullptr);
    if (FAILED(dc->EndDraw())) return {};
    dc->SetTarget(nullptr);

    D3D11_TEXTURE2D_DESC sdesc = desc;
    sdesc.Usage = D3D11_USAGE_STAGING;
    sdesc.BindFlags = 0;
    sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(d3d->CreateTexture2D(&sdesc, nullptr, &staging))) return {};
    d3d_ctx->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(d3d_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return {};
    std::vector<unsigned char> out((size_t)dst * dst * 4, 0);
    for (int y = 0; y < dst; ++y) {
        memcpy(&out[(size_t)y * dst * 4],
               static_cast<const unsigned char*>(mapped.pData) + (size_t)y * mapped.RowPitch,
               (size_t)dst * 4);
    }
    d3d_ctx->Unmap(staging.Get(), 0);
    return out;
}

int main() {
    g_log = _wfopen(L"interp_check2.log", L"w");
    if (!g_log) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3d_ctx;
    D3D_FEATURE_LEVEL level{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 &d3d, &level, &d3d_ctx))) {
        L("no d3d device");
        return 3;
    }
    ComPtr<IDXGIDevice> dxgi;
    d3d.As(&dxgi);
    ComPtr<ID2D1Factory1> factory;
    D2D1_FACTORY_OPTIONS options{};
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                      reinterpret_cast<void**>(factory.GetAddressOf()));
    ComPtr<ID2D1Device> d2d_device;
    factory->CreateDevice(dxgi.Get(), &d2d_device);
    ComPtr<ID2D1DeviceContext> dc;
    d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc);

    const int DST = 96;
    struct Mode {
        const char* name;
        D2D1_INTERPOLATION_MODE mode;
    };
    const Mode modes[] = {
        {"LINEAR", D2D1_INTERPOLATION_MODE_LINEAR},
        {"MULTI_SAMPLE_LINEAR", D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR},
        {"HIGH_QUALITY_CUBIC", D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC},
        {"ANISOTROPIC", D2D1_INTERPOLATION_MODE_ANISOTROPIC},
    };
    const D2D1_BITMAP_PROPERTIES src_props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);

    // --- Test 1: the checkerboard. Only a filter that averages the whole footprint can
    // turn 1px alternation into flat grey; anything else folds it into visible moire.
    {
        Img checker;
        checker.w = 256;
        checker.h = 256;
        checker.px.assign((size_t)256 * 256 * 4, 0);
        for (int y = 0; y < 256; ++y) {
            for (int x = 0; x < 256; ++x) {
                const unsigned char v = ((x + y) & 1) ? 255 : 0;
                unsigned char* p = &checker.px[((size_t)y * 256 + x) * 4];
                p[0] = p[1] = p[2] = v;
                p[3] = 255;
            }
        }
        ComPtr<ID2D1Bitmap> bitmap;
        dc->CreateBitmap(D2D1::SizeU(256, 256), checker.px.data(), 256 * 4, &src_props, &bitmap);
        L("--- checkerboard 256->96 (ideal: flat 127, zero variance)");
        for (const Mode& m : modes) {
            const std::vector<unsigned char> out = RenderMode(dc.Get(), bitmap.Get(), d3d.Get(),
                                                              d3d_ctx.Get(), DST, m.mode);
            if (out.empty()) { L("%-20s render failed", m.name); continue; }
            double mean = 0.0, var = 0.0;
            int minv = 255, maxv = 0;
            for (int i = 0; i < DST * DST; ++i) {
                const int v = out[(size_t)i * 4 + 1];
                mean += v;
                if (v < minv) minv = v;
                if (v > maxv) maxv = v;
            }
            mean /= (double)DST * DST;
            for (int i = 0; i < DST * DST; ++i) {
                const double d = out[(size_t)i * 4 + 1] - mean;
                var += d * d;
            }
            var /= (double)DST * DST;
            L("%-20s mean=%6.2f stdev=%6.2f min=%3d max=%3d", m.name, mean, std::sqrt(var), minv,
              maxv);
        }
    }

    // --- Test 2: distance from the reference renditions of a real icon.
    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring notepad = std::wstring(win) + L"\\System32\\notepad.exe";
    Img big, shell96;
    if (GrabIcon(notepad.c_str(), 256, big) && GrabIcon(notepad.c_str(), 96, shell96)) {
        const Img box = BoxDownscale(big, DST);
        ComPtr<ID2D1Bitmap> bitmap;
        dc->CreateBitmap(D2D1::SizeU((UINT32)big.w, (UINT32)big.h), big.px.data(),
                         (UINT32)big.w * 4, &src_props, &bitmap);
        L("--- notepad 256->96 vs references (lower rms is closer)");
        for (const Mode& m : modes) {
            const std::vector<unsigned char> out = RenderMode(dc.Get(), bitmap.Get(), d3d.Get(),
                                                              d3d_ctx.Get(), DST, m.mode);
            if (out.empty()) { L("%-20s render failed", m.name); continue; }
            L("%-20s rms_shell96=%6.3f rms_box=%6.3f", m.name, Rms(out, shell96.px),
              Rms(out, box.px));
        }
        L("reference agreement: rms_shell96_vs_box=%.3f", Rms(shell96.px, box.px));
    } else {
        L("notepad grab failed");
    }

    L("verdict: done");
    CoUninitialize();
    fclose(g_log);
    return 0;
}