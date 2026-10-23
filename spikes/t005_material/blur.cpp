// spikes/t005_material/blur.cpp - self-drawn frosted plate spike (T-005, part 3).
//
// Part 1 measured that no system material respects a custom 22px corner radius on
// a WS_EX_NOREDIRECTIONBITMAP DComp window: Mica and the accent-policy acrylic both
// paint the full window rect, ignore the DComp surface's per-pixel alpha, and
// ignore SetWindowRgn. DWM's own DWMWCP_ROUND applies a fixed small radius, not 22px.
//
// Conclusion: the frosted plate has to be drawn by us. That needs three things to
// hold, and this spike measures all three:
//   1. Capture the desktop behind the window WITHOUT capturing our own window
//      (part 2 proved WDA_EXCLUDEFROMCAPTURE does this; re-verified here per frame).
//   2. Blur it with D2D and clip to a rounded geometry.
//   3. Do that fast enough for 60 FPS at Dock size.
//
// Timing is the point. If capture+blur exceeds the frame budget, the plate must be
// cached and refreshed on a slower cadence instead of every frame.
#include <windows.h>

#include <d2d1_3.h>
#include <d2d1effects.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"DockForgeSpikeBlurDock";
constexpr int kX = 360;
constexpr int kY = 360;
constexpr int kLogicalW = 640;
constexpr int kLogicalH = 120;
constexpr float kCorner = 22.0f;

struct State {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> device;
    ComPtr<IDCompositionDesktopDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionSurface> surface;
    ComPtr<ID2D1Effect> blur;
    // Capture surface: a DIB we BitBlt into, wrapped once as a D2D bitmap.
    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old_obj = nullptr;
    void* bits = nullptr;
    int cap_w = 0;
    int cap_h = 0;
    ComPtr<ID2D1Bitmap1> cap_bitmap;
};

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool BuildComposition(State& s) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                 ARRAYSIZE(levels), D3D11_SDK_VERSION, &s.d3d, nullptr, nullptr)))
        return false;
    if (FAILED(s.d3d.As(&s.dxgi))) return false;

    D2D1_FACTORY_OPTIONS options{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 &options,
                                 reinterpret_cast<void**>(s.factory.GetAddressOf()))))
        return false;
    if (FAILED(s.factory->CreateDevice(s.dxgi.Get(), &s.device))) return false;
    if (FAILED(DCompositionCreateDevice2(s.device.Get(), IID_PPV_ARGS(&s.dcomp)))) return false;
    if (FAILED(s.dcomp->CreateTargetForHwnd(s.hwnd, TRUE, &s.target))) return false;
    if (FAILED(s.dcomp->CreateVisual(&s.visual))) return false;
    if (FAILED(s.dcomp->CreateSurface(kLogicalW, kLogicalH, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      DXGI_ALPHA_MODE_PREMULTIPLIED, &s.surface)))
        return false;
    if (FAILED(s.visual->SetContent(s.surface.Get()))) return false;
    if (FAILED(s.target->SetRoot(s.visual.Get()))) return false;
    return true;
}

bool CreateCaptureSurface(State& s, int w, int h) {
    HDC screen = GetDC(nullptr);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = w;
    bih.biHeight = -h;  // top-down, so row 0 is the top of the screen
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    s.dib = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS,
                             &s.bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!s.dib) return false;
    s.mem_dc = CreateCompatibleDC(nullptr);
    s.old_obj = SelectObject(s.mem_dc, s.dib);
    s.cap_w = w;
    s.cap_h = h;
    return true;
}

// BitBlt the screen rectangle under the window into the DIB. Returns false if GDI
// refuses; the caller keeps the previous frame in that case (graceful degradation).
bool CaptureBehind(State& s) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    const BOOL ok = BitBlt(s.mem_dc, 0, 0, s.cap_w, s.cap_h, screen, kX, kY, SRCCOPY);
    ReleaseDC(nullptr, screen);
    return ok != FALSE;
}

bool PrepareBlur(State& s, ID2D1DeviceContext* dc) {
    if (!s.cap_bitmap) {
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(s.cap_w),
                                             static_cast<UINT32>(s.cap_h));
        if (FAILED(dc->CreateBitmap(size, s.bits, static_cast<UINT32>(s.cap_w * 4), props,
                                    &s.cap_bitmap)))
            return false;
    } else {
        // Same allocation, new pixels: CopyFromMemory is the cheap path.
        const D2D1_RECT_U rect = D2D1::RectU(0, 0, static_cast<UINT32>(s.cap_w),
                                             static_cast<UINT32>(s.cap_h));
        if (FAILED(s.cap_bitmap->CopyFromMemory(&rect, s.bits, static_cast<UINT32>(s.cap_w * 4))))
            return false;
    }

    if (!s.blur) {
        if (FAILED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &s.blur))) return false;
        s.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 18.0f);
        s.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    }
    s.blur->SetInput(0, s.cap_bitmap.Get());
    return true;
}

std::int64_t Qpc() {
    LARGE_INTEGER v{};
    QueryPerformanceCounter(&v);
    return v.QuadPart;
}

double Ms(std::int64_t ticks, std::int64_t freq) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

// Isolates the GDI readback cost from everything else. BitBlt from the screen DC is
// a synchronous readback of the composited desktop; if it is a fixed sync cost the
// capture region size will not matter, and only caching or a GPU capture path helps.
void CaptureBench() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const int sizes[][2] = {{160, 60}, {320, 120}, {640, 120}, {1280, 240}, {2560, 480}};
    std::printf("region        fresh_dc_ms  cached_dc_ms  pixels\n");
    for (const auto& sz : sizes) {
        const int w = sz[0];
        const int h = sz[1];
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(bih);
        bih.biWidth = w;
        bih.biHeight = -h;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        void* bits = nullptr;
        HDC screen0 = GetDC(nullptr);
        HBITMAP dib = CreateDIBSection(screen0, reinterpret_cast<BITMAPINFO*>(&bih),
                                       DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, screen0);
        if (!dib) continue;
        HDC mem = CreateCompatibleDC(nullptr);
        HGDIOBJ old = SelectObject(mem, dib);

        // Path A: GetDC/ReleaseDC per call, as the naive implementation does.
        double fresh_sum = 0.0;
        for (int i = 0; i < 5; ++i) {
            const std::int64_t t0 = Qpc();
            HDC screen = GetDC(nullptr);
            BitBlt(mem, 0, 0, w, h, screen, kX, kY, SRCCOPY);
            ReleaseDC(nullptr, screen);
            fresh_sum += Ms(Qpc() - t0, f.QuadPart);
        }

        // Path B: one long-lived screen DC.
        double cached_sum = 0.0;
        HDC screen = GetDC(nullptr);
        for (int i = 0; i < 5; ++i) {
            const std::int64_t t0 = Qpc();
            BitBlt(mem, 0, 0, w, h, screen, kX, kY, SRCCOPY);
            cached_sum += Ms(Qpc() - t0, f.QuadPart);
        }
        ReleaseDC(nullptr, screen);

        std::printf("%4dx%-4d     %9.3f     %9.3f     %d\n", w, h, fresh_sum / 5.0,
                    cached_sum / 5.0, w * h);
        SelectObject(mem, old);
        DeleteDC(mem);
        DeleteObject(dib);
    }
}

// One full frame: capture -> blur -> clip to rounded rect -> tint -> present.
// Phase timing is reported separately because the three phases have completely
// different optimisation paths (GDI readback vs GPU blur vs compositor commit).
bool PaintFrame(State& s, std::int64_t* t_capture = nullptr, std::int64_t* t_paint = nullptr) {
    const std::int64_t c0 = Qpc();
    if (!CaptureBehind(s)) return false;
    const std::int64_t c1 = Qpc();
    if (t_capture) *t_capture = c1 - c0;

    ComPtr<ID2D1DeviceContext> dc;
    POINT offset{};
    if (FAILED(s.surface->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &offset))) return false;
    dc->SetDpi(96.0f, 96.0f);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                   static_cast<float>(offset.y)));
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (!PrepareBlur(s, dc.Get())) {
        dc->SetTarget(nullptr);
        s.surface->EndDraw();
        return false;
    }

    // Rounded clip: PushLayer with a geometry mask is the D2D way to get a
    // non-rectangular plate. The blur output is drawn 1:1 over the window rect,
    // so the captured pixels line up with what the user sees behind it.
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    s.factory->CreateRoundedRectangleGeometry(
        D2D1::RoundedRect(D2D1::RectF(0.0f, 0.0f, static_cast<float>(kLogicalW),
                                      static_cast<float>(kLogicalH)),
                          kCorner, kCorner),
        &geo);
    if (geo) {
        ComPtr<ID2D1Layer> layer;
        dc->CreateLayer(nullptr, &layer);
        const D2D1_LAYER_PARAMETERS1 params =
            D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get());
        dc->PushLayer(params, layer.Get());
    }

    dc->DrawImage(s.blur.Get(), D2D1::Point2F(0.0f, 0.0f));

    // Tint + edge: what makes it read as a plate rather than a hole.
    ComPtr<ID2D1SolidColorBrush> tint;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.09f, 0.10f, 0.12f, 0.55f), &tint);
    dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, static_cast<float>(kLogicalW),
                                  static_cast<float>(kLogicalH)),
                      tint.Get());
    if (geo) dc->PopLayer();

    ComPtr<ID2D1SolidColorBrush> border;
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f), &border);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(0.5f, 0.5f, static_cast<float>(kLogicalW) - 0.5f,
                                          static_cast<float>(kLogicalH) - 0.5f),
                              kCorner, kCorner};
    dc->DrawRoundedRectangle(&rr, border.Get(), 1.0f);

    dc->SetTarget(nullptr);
    if (FAILED(s.surface->EndDraw())) return false;
    const std::int64_t c2 = Qpc();
    if (t_paint) *t_paint = c2 - c1;
    return SUCCEEDED(s.dcomp->Commit());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int frames = 300;
    bool exclude = true;
    bool capture_bench = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--frames=", 0) == 0) frames = _wtoi(arg.c_str() + 9);
        if (arg == L"--no-exclude") exclude = false;
        if (arg == L"--capture-bench") capture_bench = true;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    if (capture_bench) {
        CaptureBench();
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 1;

    State s;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP, kClass,
                             L"blur-dock", WS_POPUP, kX, kY, kLogicalW, kLogicalH, nullptr,
                             nullptr, inst, nullptr);
    if (!s.hwnd) return 1;

    if (exclude) {
        const BOOL ok = SetWindowDisplayAffinity(s.hwnd, WDA_EXCLUDEFROMCAPTURE);
        std::printf("SetWindowDisplayAffinity = %s\n", ok ? "OK" : "FAIL");
    }

    if (!BuildComposition(s)) {
        std::printf("composition failed\n");
        return 2;
    }
    if (!CreateCaptureSurface(s, kLogicalW, kLogicalH)) {
        std::printf("capture surface failed\n");
        return 3;
    }

    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    Sleep(300);  // let DWM settle before measuring

    // Warm-up: the first frame allocates the D2D bitmap and compiles the blur
    // shader, which measured 620ms. Measuring that as steady state would be a lie.
    for (int i = 0; i < 5; ++i) PaintFrame(s);

    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(frames));
    std::vector<double> cap_samples;
    cap_samples.reserve(static_cast<std::size_t>(frames));

    int failures = 0;
    for (int i = 0; i < frames; ++i) {
        std::int64_t t_cap = 0;
        std::int64_t t_paint = 0;
        const std::int64_t t0 = Qpc();
        if (!PaintFrame(s, &t_cap, &t_paint)) ++failures;
        const std::int64_t t1 = Qpc();
        samples.push_back(Ms(t1 - t0, f.QuadPart));
        cap_samples.push_back(Ms(t_cap, f.QuadPart));
    }

    std::sort(samples.begin(), samples.end());
    std::sort(cap_samples.begin(), cap_samples.end());
    const auto pct = [&](double p) {
        const std::size_t rank = static_cast<std::size_t>(std::ceil(p / 100.0 * samples.size()));
        return samples[rank > 0 ? rank - 1 : 0];
    };
    const auto pct_cap = [&](double p) {
        const std::size_t rank =
            static_cast<std::size_t>(std::ceil(p / 100.0 * cap_samples.size()));
        return cap_samples[rank > 0 ? rank - 1 : 0];
    };
    double sum = 0.0;
    for (double v : samples) sum += v;
    double cap_sum = 0.0;
    for (double v : cap_samples) cap_sum += v;

    std::printf("frames=%d failures=%d size=%dx%d\n", frames, failures, kLogicalW, kLogicalH);
    std::printf("capture+blur+present ms: avg=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n",
                sum / static_cast<double>(samples.size()), pct(50), pct(95), pct(99),
                samples.back());
    std::printf("BitBlt phase ms:         avg=%.3f p50=%.3f p99=%.3f\n",
                cap_sum / static_cast<double>(cap_samples.size()), pct_cap(50), pct_cap(99));
    std::printf("verdict=%s\n", pct(99) < 16.7 ? "WITHIN_FRAME_BUDGET" : "NEEDS_CACHING");
    return 0;
}
