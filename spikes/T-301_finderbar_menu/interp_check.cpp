// Does the interpolation mode actually change what the user sees?
//
// icon_quality.cpp measured detail loss on the CPU, which proves the arithmetic but not
// the renderer. This spike drives the real pipeline - D3D11 texture as an
// ID2D1DeviceContext target, ID2D1DeviceContext::DrawBitmap with each
// D2D1_INTERPOLATION_MODE - and reads the result back, so the choice in
// src/render/backend_d2d.cpp rests on rendered pixels.
//
// Measured per mode:
//   detail   mean |luminance gradient| over the destination. Higher is crisper.
//   bleed    alpha summed in the outermost 3px ring, where the source is empty. Cubic
//            ringing would light it up.
//   overshoot count of destination pixels whose alpha exceeds the maximum alpha in the
//            corresponding 2x2 source neighbourhood. Cubic can overshoot; linear cannot.
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
    std::vector<unsigned char> px;  // BGRA, premultiplied, top-down
};

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
    out.px.assign(static_cast<size_t>(out.w) * out.h * 4, 0);
    HDC dc = GetDC(nullptr);
    const int rows = GetDIBits(dc, bmp, 0, (UINT)bm.bmHeight, out.px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(bmp);
    if (rows != bm.bmHeight) return false;
    // Premultiply exactly as src/shell/icon_cache.cpp now does.
    bool straight = false;
    for (size_t i = 0; i + 3 < out.px.size(); i += 4) {
        const unsigned a = out.px[i + 3];
        if (a != 0 && a != 255 && (out.px[i] > a || out.px[i + 1] > a || out.px[i + 2] > a)) {
            straight = true;
            break;
        }
    }
    if (straight) {
        for (size_t i = 0; i + 3 < out.px.size(); i += 4) {
            const unsigned a = out.px[i + 3];
            if (a == 0) {
                out.px[i] = out.px[i + 1] = out.px[i + 2] = 0;
                continue;
            }
            if (a == 255) continue;
            for (int c = 0; c < 3; ++c)
                out.px[i + c] = (unsigned char)((out.px[i + c] * a + 127u) / 255u);
        }
    }
    return true;
}

static double Luma(const unsigned char* p) {
    return 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
}

int main() {
    g_log = _wfopen(L"interp_check.log", L"w");
    if (!g_log) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // --- D3D11 + D2D device context, the same object graph the app builds.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    ComPtr<ID3D11Device> d3d;
    ComPtr<ID3D11DeviceContext> d3d_ctx;
    D3D_FEATURE_LEVEL level{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0,
                                   D3D11_SDK_VERSION, &d3d, &level, &d3d_ctx);
    if (FAILED(hr)) {
        L("D3D11CreateDevice failed 0x%08lX", (unsigned long)hr);
        return 3;
    }
    L("device feature_level=0x%04X", (unsigned)level);

    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d.As(&dxgi))) { L("no IDXGIDevice"); return 3; }
    ComPtr<ID2D1Factory1> factory;
    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 &options, reinterpret_cast<void**>(factory.GetAddressOf())))) {
        L("D2D1CreateFactory failed");
        return 3;
    }
    ComPtr<ID2D1Device> d2d_device;
    if (FAILED(factory->CreateDevice(dxgi.Get(), &d2d_device))) { L("CreateDevice failed"); return 3; }
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc))) {
        L("CreateDeviceContext failed");
        return 3;
    }

    wchar_t win[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    const std::wstring notepad = std::wstring(win) + L"\\System32\\notepad.exe";
    Img src;
    if (!GrabIcon(notepad.c_str(), 256, src)) { L("grab failed"); return 3; }
    L("source %dx%d", src.w, src.h);

    // --- destination texture, 96px = 48 dip at 192 DPI, the dock's real tile size.
    const int DST = 96;
    ComPtr<ID2D1Bitmap> src_bitmap;
    const D2D1_BITMAP_PROPERTIES src_props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    if (FAILED(dc->CreateBitmap(D2D1::SizeU((UINT32)src.w, (UINT32)src.h), src.px.data(),
                                (UINT32)src.w * 4, &src_props, &src_bitmap))) {
        L("CreateBitmap(source) failed");
        return 3;
    }

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

    for (const Mode& m : modes) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = DST;
        desc.Height = DST;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        if (FAILED(d3d->CreateTexture2D(&desc, nullptr, &texture))) { L("%s: texture failed", m.name); continue; }
        ComPtr<IDXGISurface> surface;
        if (FAILED(texture.As(&surface))) { L("%s: no surface", m.name); continue; }
        const D2D1_BITMAP_PROPERTIES1 target_props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f);
        ComPtr<ID2D1Bitmap1> target;
        if (FAILED(dc->CreateBitmapFromDxgiSurface(surface.Get(), &target_props, &target))) {
            L("%s: target bitmap failed", m.name);
            continue;
        }
        dc->SetTarget(target.Get());
        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0.0f));
        dc->DrawBitmap(src_bitmap.Get(), D2D1::RectF(0, 0, (float)DST, (float)DST), 1.0f,
                       m.mode, nullptr, nullptr);
        const HRESULT end = dc->EndDraw();
        if (FAILED(end)) { L("%s: EndDraw 0x%08lX", m.name, (unsigned long)end); continue; }
        dc->SetTarget(nullptr);

        D3D11_TEXTURE2D_DESC sdesc = desc;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.BindFlags = 0;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(d3d->CreateTexture2D(&sdesc, nullptr, &staging))) { L("%s: staging failed", m.name); continue; }
        d3d_ctx->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d3d_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            L("%s: map failed", m.name);
            continue;
        }
        std::vector<unsigned char> out((size_t)DST * DST * 4, 0);
        for (int y = 0; y < DST; ++y) {
            memcpy(&out[(size_t)y * DST * 4],
                   static_cast<const unsigned char*>(mapped.pData) + (size_t)y * mapped.RowPitch,
                   (size_t)DST * 4);
        }
        d3d_ctx->Unmap(staging.Get(), 0);

        double detail = 0.0;
        for (int y = 1; y < DST; ++y) {
            for (int x = 1; x < DST; ++x) {
                const double c = Luma(&out[((size_t)y * DST + x) * 4]);
                detail += std::fabs(c - Luma(&out[((size_t)(y - 1) * DST + x) * 4]));
                detail += std::fabs(c - Luma(&out[((size_t)y * DST + x - 1) * 4]));
            }
        }
        detail /= (double)DST * DST;

        long long bleed = 0;
        for (int y = 0; y < DST; ++y) {
            for (int x = 0; x < DST; ++x) {
                const bool ring = x < 3 || y < 3 || x >= DST - 3 || y >= DST - 3;
                if (ring) bleed += out[((size_t)y * DST + x) * 4 + 3];
            }
        }

        int overshoot = 0;
        const double scale = (double)src.w / DST;
        for (int y = 0; y < DST; ++y) {
            for (int x = 0; x < DST; ++x) {
                const int a = out[((size_t)y * DST + x) * 4 + 3];
                int neighbourhood_max = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        int sx = (int)(x * scale) + dx;
                        int sy = (int)(y * scale) + dy;
                        if (sx >= src.w) sx = src.w - 1;
                        if (sy >= src.h) sy = src.h - 1;
                        const int sa = src.px[((size_t)sy * src.w + sx) * 4 + 3];
                        if (sa > neighbourhood_max) neighbourhood_max = sa;
                    }
                }
                if (a > neighbourhood_max + 2) ++overshoot;
            }
        }
        L("%-20s detail=%.3f bleed=%lld overshoot=%d", m.name, detail, bleed, overshoot);
    }
    L("verdict: done");
    CoUninitialize();
    fclose(g_log);
    return 0;
}