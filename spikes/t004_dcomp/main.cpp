// spikes/t004_dcomp/main.cpp - DirectComposition layered-window spike (T-004).
//
// Questions this spike answers with measurements, not memory:
//   1. Which object must back DCompositionCreateDevice2 so that
//      IDCompositionSurface::BeginDraw can hand back an ID2D1DeviceContext?
//   2. Do DComp surfaces composite with per-pixel alpha (rounded corner really
//      transparent) under WS_EX_NOREDIRECTIONBITMAP, and does adding
//      WS_EX_LAYERED change anything?
//   3. Is the content DPI-correct: does the same logical layout render at
//      proportionally identical physical size when the effective DPI changes?
//
// The window is placed at a fixed screen position so an external pixel sampler
// can read exact coordinates. A coloured backdrop window sits behind it, because
// this machine's wallpaper is pure black and "transparent" would otherwise be
// indistinguishable from "painted black".
//
// Layout is authored in DIPs and scaled by dpi/96, which is the contract C-04
// requires from every renderer.
#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClassName[] = L"DockForgeSpikeT004";
constexpr wchar_t kBackdropClass[] = L"DockForgeSpikeT004Backdrop";

// Logical (DIP) geometry. Physical size follows the effective DPI.
constexpr float kLogicalW = 240.0f;
constexpr float kLogicalH = 90.0f;
constexpr float kLogicalCorner = 20.0f;
constexpr int kWindowX = 160;
constexpr int kWindowY = 160;
constexpr COLORREF kBackdropColor = RGB(0, 128, 255);

struct State {
    HWND hwnd = nullptr;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<ID2D1Factory1> d2d_factory;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<IDCompositionDesktopDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionSurface> surface;
    UINT surface_w = 0;
    UINT surface_h = 0;
};

void Report(const char* step, HRESULT hr) {
    std::printf("%-48s hr=0x%08lX %s\n", step, static_cast<unsigned long>(hr),
                SUCCEEDED(hr) ? "OK" : "FAIL");
    std::fflush(stdout);
}

HRESULT CreateD3DDevice(State& s) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &s.d3d, nullptr, nullptr);
    Report("D3D11CreateDevice(hardware)", hr);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &s.d3d, nullptr, nullptr);
        Report("D3D11CreateDevice(warp)", hr);
        if (FAILED(hr)) return hr;
    }
    hr = s.d3d.As(&s.dxgi_device);
    Report("ID3D11Device -> IDXGIDevice", hr);
    return hr;
}

HRESULT CreateD2D(State& s) {
    D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                   &options,
                                   reinterpret_cast<void**>(s.d2d_factory.GetAddressOf()));
    Report("D2D1CreateFactory", hr);
    if (FAILED(hr)) return hr;
    hr = s.d2d_factory->CreateDevice(s.dxgi_device.Get(), &s.d2d_device);
    Report("ID2D1Factory1::CreateDevice", hr);
    return hr;
}

HRESULT CreateComposition(State& s, UINT width, UINT height) {
    // The DComp device must be built from the ID2D1Device, not the raw
    // IDXGIDevice. With IDXGIDevice every call up to Commit succeeds, then
    // BeginDraw fails with E_NOINTERFACE (0x80004002) — measured on this host.
    HRESULT hr = DCompositionCreateDevice2(s.d2d_device.Get(), IID_PPV_ARGS(&s.dcomp));
    Report("DCompositionCreateDevice2(ID2D1Device)", hr);
    if (FAILED(hr)) return hr;

    hr = s.dcomp->CreateTargetForHwnd(s.hwnd, TRUE, &s.target);
    Report("IDCompositionDesktopDevice::CreateTargetForHwnd", hr);
    if (FAILED(hr)) return hr;

    hr = s.dcomp->CreateVisual(&s.visual);
    Report("IDCompositionDevice2::CreateVisual", hr);
    if (FAILED(hr)) return hr;

    hr = s.dcomp->CreateSurface(width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_ALPHA_MODE_PREMULTIPLIED, &s.surface);
    Report("IDCompositionDevice2::CreateSurface", hr);
    if (FAILED(hr)) return hr;
    s.surface_w = width;
    s.surface_h = height;

    hr = s.visual->SetContent(s.surface.Get());
    Report("IDCompositionVisual::SetContent", hr);
    if (FAILED(hr)) return hr;

    hr = s.target->SetRoot(s.visual.Get());
    Report("IDCompositionTarget::SetRoot", hr);
    return hr;
}

HRESULT Paint(State& s, float dpi) {
    ComPtr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = s.surface->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &offset);
    Report("IDCompositionSurface::BeginDraw(ID2D1DeviceContext)", hr);
    if (FAILED(hr)) return hr;

    // Draw in DIPs: the context DPI converts to physical pixels, so the same
    // logical geometry lands at the same relative place on every screen.
    dc->SetDpi(dpi, dpi);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                  static_cast<float>(offset.y)));
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    ComPtr<ID2D1SolidColorBrush> brush;
    hr = dc->CreateSolidColorBrush(D2D1::ColorF(0.98f, 0.22f, 0.25f, 1.0f), &brush);
    Report("ID2D1DeviceContext::CreateSolidColorBrush", hr);
    if (FAILED(hr)) {
        s.surface->EndDraw();
        return hr;
    }

    const D2D1_ROUNDED_RECT rr{D2D1::RectF(0.0f, 0.0f, kLogicalW, kLogicalH), kLogicalCorner,
                               kLogicalCorner};
    dc->FillRoundedRectangle(rr, brush.Get());
    dc->SetTarget(nullptr);

    hr = s.surface->EndDraw();
    Report("IDCompositionSurface::EndDraw", hr);
    return hr;
}

DWORD ParseMode(const wchar_t* arg) {
    if (std::wcsstr(arg, L"plain")) return 0;
    if (std::wcsstr(arg, L"both")) return WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED;
    if (std::wcsstr(arg, L"layered")) return WS_EX_LAYERED;
    return WS_EX_NOREDIRECTIONBITMAP;
}

const char* ModeName(DWORD ex_style) {
    const bool noredir = (ex_style & WS_EX_NOREDIRECTIONBITMAP) != 0;
    const bool layered = (ex_style & WS_EX_LAYERED) != 0;
    if (noredir && layered) return "NOREDIRECTIONBITMAP|LAYERED";
    if (layered) return "LAYERED";
    if (noredir) return "NOREDIRECTIONBITMAP";
    return "PLAIN";
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HBRUSH brush = CreateSolidBrush(kBackdropColor);
            FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
            DeleteObject(brush);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            HBRUSH brush = CreateSolidBrush(kBackdropColor);
            FillRect(dc, &rc, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    DWORD ex_style = WS_EX_NOREDIRECTIONBITMAP;
    int hold_ms = 6000;
    bool backdrop = false;
    float forced_dpi = 0.0f;
    for (int i = 1; i < argc; ++i) {
        if (std::wcsncmp(argv[i], L"--mode=", 7) == 0) {
            ex_style = ParseMode(argv[i]);
        } else if (std::wcsncmp(argv[i], L"--hold-ms=", 10) == 0) {
            hold_ms = _wtoi(argv[i] + 10);
        } else if (std::wcscmp(argv[i], L"--backdrop") == 0) {
            backdrop = true;
        } else if (std::wcsncmp(argv[i], L"--dpi=", 6) == 0) {
            forced_dpi = static_cast<float>(_wtoi(argv[i] + 6));
        }
    }

    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Report("CoInitializeEx", com_hr);

    State state;
    if (FAILED(CreateD3DDevice(state))) return 10;
    if (FAILED(CreateD2D(state))) return 11;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        std::printf("RegisterClassExW failed err=%lu\n", GetLastError());
        return 12;
    }

    // Physical window size derives from the DPI the content will be drawn at.
    // Without a window we ask the monitor directly, which is also what the
    // real Dock will do before it has an HWND.
    UINT monitor_dpi = 96;
    {
        POINT pt{kWindowX, kWindowY};
        HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        if (monitor) {
            UINT x = 96, y = 96;
            if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) {
                monitor_dpi = x;
            }
        }
    }
    const float dpi = forced_dpi > 0.0f ? forced_dpi : static_cast<float>(monitor_dpi);
    const float scale = dpi / 96.0f;
    const UINT phys_w = static_cast<UINT>(std::lround(kLogicalW * scale));
    const UINT phys_h = static_cast<UINT>(std::lround(kLogicalH * scale));
    std::printf("mode=%s dpi=%.0f scale=%.3f physical=%ux%u origin=(%d,%d)\n", ModeName(ex_style),
                static_cast<double>(dpi), static_cast<double>(scale), phys_w, phys_h, kWindowX,
                kWindowY);

    HWND backdrop_hwnd = nullptr;
    if (backdrop) {
        WNDCLASSEXW bwc{};
        bwc.cbSize = sizeof(bwc);
        bwc.lpfnWndProc = &BackdropProc;
        bwc.hInstance = wc.hInstance;
        bwc.lpszClassName = kBackdropClass;
        RegisterClassExW(&bwc);
        // Not topmost: the spike window is topmost and must stay above it.
        backdrop_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW, kBackdropClass, L"", WS_POPUP, kWindowX - 24, kWindowY - 24,
            static_cast<int>(phys_w) + 48, static_cast<int>(phys_h) + 48, nullptr, nullptr,
            wc.hInstance, nullptr);
        if (backdrop_hwnd) {
            ShowWindow(backdrop_hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(backdrop_hwnd);
            std::printf("backdrop=on color=RGB(0,128,255)\n");
        }
    }

    state.hwnd = CreateWindowExW(ex_style | WS_EX_TOPMOST, kClassName, L"DockForge T-004 spike",
                                 WS_POPUP, kWindowX, kWindowY, static_cast<int>(phys_w),
                                 static_cast<int>(phys_h), nullptr, nullptr, wc.hInstance,
                                 nullptr);
    if (!state.hwnd) {
        std::printf("CreateWindowExW failed err=%lu\n", GetLastError());
        return 13;
    }
    std::printf("window_dpi=%u\n", GetDpiForWindow(state.hwnd));

    if (FAILED(CreateComposition(state, phys_w, phys_h))) return 14;
    if (FAILED(Paint(state, dpi))) return 15;

    const HRESULT commit_hr = state.dcomp->Commit();
    Report("IDCompositionDevice2::Commit", commit_hr);
    if (FAILED(commit_hr)) return 16;

    ShowWindow(state.hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(state.hwnd);
    SetTimer(state.hwnd, 1, static_cast<UINT>(hold_ms > 0 ? hold_ms : 6000), nullptr);

    std::printf("READY\n");
    std::fflush(stdout);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    state.surface.Reset();
    state.visual.Reset();
    state.target.Reset();
    state.dcomp.Reset();
    state.d2d_device.Reset();
    state.d2d_factory.Reset();
    state.dxgi_device.Reset();
    state.d3d.Reset();
    if (backdrop_hwnd) DestroyWindow(backdrop_hwnd);
    if (SUCCEEDED(com_hr)) CoUninitialize();
    std::printf("EXIT\n");
    return 0;
}
