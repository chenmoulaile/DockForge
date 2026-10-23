// spikes/t005_material/main.cpp - backdrop material spike (T-005).
//
// Question: on a WS_EX_NOREDIRECTIONBITMAP + DirectComposition window, do the
// system backdrop materials actually apply?
//   - Mica / Acrylic via DwmSetWindowAttribute(DWMWA_SYSTEMBACKDROP_TYPE)
//   - Acrylic / blur via user32!SetWindowCompositionAttribute (undocumented)
//   - DwmEnableBlurBehindWindow (Vista-era)
//
// The answer decides whether the Dock's material fallback chain can lean on the
// OS at all, or whether the frosted look has to be drawn by us.
//
// Method: a solid-blue backdrop window sits behind a translucent DComp panel. If
// a system material is active, the panel interior picks up blue tint/blur. If it
// is inert, the interior stays neutral grey. Sampling is done externally
// (tools/sample_pixels.ps1) so the verdict is not self-reported.
#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kDockClass[] = L"DockForgeSpikeT005Dock";
constexpr wchar_t kBackdropClass[] = L"DockForgeSpikeT005Backdrop";
constexpr int kWinX = 200;
constexpr int kWinY = 200;
constexpr int kWinW = 480;
constexpr int kWinH = 200;

// --- undocumented user32 composition API -------------------------------
// Verified present on this host: GetProcAddress(user32, "SetWindowCompositionAttribute")
// returns a non-null pointer. Declared locally because no SDK header ships it.
enum ACCENT_STATE : DWORD {
    ACCENT_DISABLED = 0,
    ACCENT_ENABLE_GRADIENT = 1,
    ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
    ACCENT_ENABLE_BLURBEHIND = 3,
    ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
    ACCENT_ENABLE_HOSTBACKDROP = 5,
};

struct ACCENT_POLICY {
    DWORD AccentState;
    DWORD AccentFlags;
    DWORD GradientColor;  // ABGR
    DWORD AnimationId;
};

enum WINDOWCOMPOSITIONATTRIB : DWORD { WCA_ACCENT_POLICY = 19 };

struct WINDOWCOMPOSITIONATTRIBDATA {
    WINDOWCOMPOSITIONATTRIB Attrib;
    PVOID pvData;
    SIZE_T cbData;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WINDOWCOMPOSITIONATTRIBDATA*);

struct State {
    HWND dock = nullptr;
    HWND backdrop = nullptr;
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> d2d_factory;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<IDCompositionDesktopDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionSurface> surface;
};

void Report(const char* step, HRESULT hr) {
    std::printf("%-52s hr=0x%08lX %s\n", step, static_cast<unsigned long>(hr),
                SUCCEEDED(hr) ? "OK" : "FAIL");
    std::fflush(stdout);
}

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            // High-frequency checkerboard, not a flat colour: a flat backdrop can
            // prove "the window is translucent" but never "the backdrop is
            // blurred". With 8px cells, blur destroys the pattern and a plain
            // alpha blend preserves it — measurable by sampling a row.
            HDC dc = reinterpret_cast<HDC>(wp);
            HBRUSH blue = CreateSolidBrush(RGB(0, 128, 255));
            HBRUSH dark = CreateSolidBrush(RGB(0, 32, 96));
            const int cell = 8;
            for (int y = 0; y < rc.bottom; y += cell) {
                for (int x = 0; x < rc.right; x += cell) {
                    RECT c{x, y, x + cell, y + cell};
                    FillRect(dc, &c, (((x / cell) + (y / cell)) % 2) ? dark : blue);
                }
            }
            DeleteObject(blue);
            DeleteObject(dark);
            return 1;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT CALLBACK DockProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool CreateBackdrop(HINSTANCE inst, State& s) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = inst;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kBackdropClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    // Deliberately NOT topmost: ADR-0007 records that two topmost windows stack in
    // an order unrelated to creation order, and this spike needs the backdrop
    // reliably behind.
    s.backdrop = CreateWindowExW(WS_EX_TOOLWINDOW, kBackdropClass, L"backdrop", WS_POPUP,
                                 kWinX - 40, kWinY - 40, kWinW + 80, kWinH + 80, nullptr,
                                 nullptr, inst, nullptr);
    if (!s.backdrop) return false;
    ShowWindow(s.backdrop, SW_SHOWNOACTIVATE);
    UpdateWindow(s.backdrop);
    return true;
}

bool CreateDock(HINSTANCE inst, State& s, bool use_dcomp) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DockProc;
    wc.hInstance = inst;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kDockClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    if (use_dcomp) ex |= WS_EX_NOREDIRECTIONBITMAP;

    s.dock = CreateWindowExW(ex, kDockClass, L"dock", WS_POPUP, kWinX, kWinY, kWinW, kWinH,
                             nullptr, nullptr, inst, nullptr);
    return s.dock != nullptr;
}

bool CreateCompositionStack(State& s) {
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &s.d3d, nullptr, nullptr);
    Report("D3D11CreateDevice", hr);
    if (FAILED(hr)) return false;

    hr = s.d3d.As(&s.dxgi);
    Report("ID3D11Device -> IDXGIDevice", hr);
    if (FAILED(hr)) return false;

    D2D1_FACTORY_OPTIONS options{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                           reinterpret_cast<void**>(s.d2d_factory.GetAddressOf()));
    Report("D2D1CreateFactory", hr);
    if (FAILED(hr)) return false;

    hr = s.d2d_factory->CreateDevice(s.dxgi.Get(), &s.d2d_device);
    Report("ID2D1Factory1::CreateDevice", hr);
    if (FAILED(hr)) return false;

    hr = DCompositionCreateDevice2(s.d2d_device.Get(), IID_PPV_ARGS(&s.dcomp));
    Report("DCompositionCreateDevice2", hr);
    if (FAILED(hr)) return false;

    hr = s.dcomp->CreateTargetForHwnd(s.dock, TRUE, &s.target);
    Report("CreateTargetForHwnd", hr);
    if (FAILED(hr)) return false;

    hr = s.dcomp->CreateVisual(&s.visual);
    Report("CreateVisual", hr);
    if (FAILED(hr)) return false;

    hr = s.dcomp->CreateSurface(kWinW, kWinH, DXGI_FORMAT_B8G8R8A8_UNORM,
                                DXGI_ALPHA_MODE_PREMULTIPLIED, &s.surface);
    Report("CreateSurface", hr);
    if (FAILED(hr)) return false;

    hr = s.visual->SetContent(s.surface.Get());
    Report("IDCompositionVisual::SetContent", hr);
    if (FAILED(hr)) return false;

    hr = s.target->SetRoot(s.visual.Get());
    Report("IDCompositionTarget::SetRoot", hr);
    if (FAILED(hr)) return false;
    return true;
}

void PaintPanel(State& s, float alpha, bool full_rect) {
    ComPtr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = s.surface->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &offset);
    if (FAILED(hr)) {
        Report("BeginDraw", hr);
        return;
    }
    dc->SetDpi(96.0f, 96.0f);
    // BeginDraw reports where this surface sits INSIDE the shared atlas, in
    // physical pixels. Ignoring it draws at atlas (0,0) and the window shows
    // whatever else lives there — measured as "panel completely missing".
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                   static_cast<float>(offset.y)));
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    ComPtr<ID2D1SolidColorBrush> fill;
    dc->CreateSolidColorBrush(D2D1::ColorF(0.094f, 0.102f, 0.125f, alpha), fill.GetAddressOf());
    ComPtr<ID2D1SolidColorBrush> border;
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f), border.GetAddressOf());
    ComPtr<ID2D1SolidColorBrush> marker;
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f), marker.GetAddressOf());

    const D2D1_ROUNDED_RECT rr{D2D1::RectF(2.0f, 2.0f, static_cast<float>(kWinW) - 2.0f,
                                          static_cast<float>(kWinH) - 2.0f),
                               24.0f, 24.0f};
    if (full_rect) {
        // Fill the whole client rect, so "is the corner clipped" measures the
        // window region, not the shape we drew.
        dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, static_cast<float>(kWinW),
                                      static_cast<float>(kWinH)),
                          fill.Get());
    } else {
        dc->FillRoundedRectangle(&rr, fill.Get());
        dc->DrawRoundedRectangle(&rr, border.Get(), 1.0f);
    }
    // Unconditional opaque marker: proves the D2D content reached the screen at
    // all, independent of how the material interacts with the translucent fill.
    dc->FillRectangle(D2D1::RectF(16.0f, 16.0f, 64.0f, 64.0f), marker.Get());

    dc->SetTarget(nullptr);
    Report("EndDraw", s.surface->EndDraw());
    Report("Commit", s.dcomp->Commit());
}

void ApplyMaterial(State& s, const std::string& material) {
    if (material == "mica" || material == "acrylic_dwm") {
        const int backdrop =
            material == "mica" ? DWMSBT_MAINWINDOW : DWMSBT_TRANSIENTWINDOW;
        HRESULT hr = DwmSetWindowAttribute(s.dock, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop,
                                           sizeof(backdrop));
        Report(material == "mica" ? "DwmSetWindowAttribute(DWMSBT_MAINWINDOW)"
                                  : "DwmSetWindowAttribute(DWMSBT_TRANSIENTWINDOW)",
               hr);
        // Mica on Win11 also wants the frame extended into the client area.
        MARGINS margins{-1, -1, -1, -1};
        Report("DwmExtendFrameIntoClientArea(-1)", DwmExtendFrameIntoClientArea(s.dock, &margins));
        return;
    }

    if (material == "acrylic" || material == "blur") {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        auto set_attr = reinterpret_cast<SetWindowCompositionAttributeFn>(
            GetProcAddress(user32, "SetWindowCompositionAttribute"));
        if (!set_attr) {
            std::printf("%-52s %s\n", "GetProcAddress(SetWindowCompositionAttribute)", "NULL");
            return;
        }
        ACCENT_POLICY policy{};
        policy.AccentState =
            material == "acrylic" ? ACCENT_ENABLE_ACRYLICBLURBEHIND : ACCENT_ENABLE_BLURBEHIND;
        policy.AccentFlags = 2;               // draw all borders
        policy.GradientColor = 0x99000000;    // AABBGGRR: 60% black tint
        WINDOWCOMPOSITIONATTRIBDATA data{};
        data.Attrib = WCA_ACCENT_POLICY;
        data.pvData = &policy;
        data.cbData = sizeof(policy);
        const BOOL ok = set_attr(s.dock, &data);
        std::printf("%-52s %s\n",
                    material == "acrylic" ? "SetWindowCompositionAttribute(ACRYLICBLURBEHIND)"
                                          : "SetWindowCompositionAttribute(BLURBEHIND)",
                    ok ? "OK" : "FAIL");
        std::fflush(stdout);
        return;
    }

    if (material == "blurbehind") {
        DWM_BLURBEHIND bb{};
        bb.dwFlags = DWM_BB_ENABLE;
        bb.fEnable = TRUE;
        Report("DwmEnableBlurBehindWindow", DwmEnableBlurBehindWindow(s.dock, &bb));
        return;
    }

    std::printf("%-52s %s\n", "material", material.c_str());
    std::fflush(stdout);
}

// The accent-policy blur paints the whole window rectangle, ignoring the alpha in
// our DComp surface AND ignoring SetWindowRgn — measured: with a rounded region in
// place the corner still sampled accent colour. So a rounded Dock needs DWM's own
// corner rounding, which is applied by the compositor above the accent layer.
void ApplyDwmCorners(HWND hwnd, int preference) {
    HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                                       sizeof(preference));
    Report(preference == DWMWCP_ROUND ? "DwmSetWindowAttribute(DWMWCP_ROUND)"
                                     : "DwmSetWindowAttribute(DWMWCP_DONOTROUND)",
           hr);
}

// The accent-policy blur paints the whole window rectangle, ignoring the alpha in
// our DComp surface — so a rounded Dock would get square blur. This test checks
// whether a rounded window region clips it.
void ApplyRoundedRegion(HWND hwnd, int w, int h, int radius) {
    HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius * 2, radius * 2);
    if (!region) {
        std::printf("%-52s %s\n", "CreateRoundRectRgn", "NULL");
        return;
    }
    // SetWindowRgn takes ownership of the region on success.
    const int ok = SetWindowRgn(hwnd, region, TRUE);
    std::printf("%-52s %s\n", "SetWindowRgn(round)", ok ? "OK" : "FAIL");
    std::fflush(stdout);
    if (!ok) DeleteObject(region);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::string material = "none";
    bool use_dcomp = true;
    bool use_backdrop = true;
    int hold_ms = 6000;
    float alpha = 0.80f;
    bool round_region = false;
    bool rgn_late = false;
    bool full_rect = false;
    int corner_pref = -1;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const auto value = [&](const wchar_t* key) -> std::wstring {
            const std::wstring prefix = std::wstring(key) + L"=";
            return arg.rfind(prefix, 0) == 0 ? arg.substr(prefix.size()) : std::wstring{};
        };
        if (auto v = value(L"--material"); !v.empty()) {
            material = std::string(v.begin(), v.end());
        } else if (auto v = value(L"--hold-ms"); !v.empty()) {
            hold_ms = _wtoi(v.c_str());
        } else if (arg == L"--no-dcomp") {
            use_dcomp = false;
        } else if (arg == L"--no-backdrop") {
            use_backdrop = false;
        } else if (auto v = value(L"--alpha"); !v.empty()) {
            alpha = static_cast<float>(_wtof(v.c_str()));
        } else if (arg == L"--round-region") {
            round_region = true;
        } else if (arg == L"--rgn-late") {
            rgn_late = true;
        } else if (arg == L"--full-rect") {
            full_rect = true;
        } else if (arg == L"--dwm-corners") {
            corner_pref = 2;
        } else if (arg == L"--dwm-corners-none") {
            corner_pref = 1;
        }
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    State s;

    // GetVersionExW lies without a manifest; RtlGetVersion reports the truth.
    ULONG build = 0;
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        // RTL_OSVERSIONINFOW is layout-identical to OSVERSIONINFOW, so casting the
        // prototype avoids pulling in winternl.h for one call.
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        auto rtl = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
        OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (rtl && rtl(&vi) == 0) build = vi.dwBuildNumber;
    }
    std::printf("host build=%lu (mica requires >=22000) dcomp=%d material=%s\n",
                static_cast<unsigned long>(build), use_dcomp ? 1 : 0, material.c_str());

    if (use_backdrop && !CreateBackdrop(inst, s)) {
        std::printf("backdrop creation failed\n");
        return 1;
    }
    if (!CreateDock(inst, s, use_dcomp)) {
        std::printf("dock creation failed\n");
        return 1;
    }

    // Material first, then composition stack: the backdrop is the window's own
    // background layer, and DComp visuals composite on top of it. Applying the
    // material afterwards still worked on this host, but this order matches how
    // the real Dock will initialise.
    ApplyMaterial(s, material);
    if (round_region && !rgn_late) ApplyRoundedRegion(s.dock, kWinW, kWinH, 24);
    if (corner_pref >= 0) ApplyDwmCorners(s.dock, corner_pref);

    if (use_dcomp) {
        if (!CreateCompositionStack(s)) return 2;
    }

    ShowWindow(s.dock, SW_SHOWNOACTIVATE);
    UpdateWindow(s.dock);
    // Re-apply after show: several DWM attributes are silently ignored while the
    // window is still hidden.
    ApplyMaterial(s, material);
    // Question under test: does the accent policy destroy the window region, so
    // that the region must be re-applied after the material?
    if (round_region && rgn_late) ApplyRoundedRegion(s.dock, kWinW, kWinH, 24);
    if (corner_pref >= 0) ApplyDwmCorners(s.dock, corner_pref);
    if (use_dcomp) PaintPanel(s, alpha, full_rect);

    std::printf("holding %d ms at %d,%d %dx%d alpha=%.2f\n", hold_ms, kWinX, kWinY, kWinW,
                kWinH, static_cast<double>(alpha));
    std::fflush(stdout);
    Sleep(static_cast<DWORD>(hold_ms));
    return 0;
}
