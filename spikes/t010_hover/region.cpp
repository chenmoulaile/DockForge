// T-010 spike, part 2: does SetWindowRgn clip a DirectComposition surface, and
// does hit-testing follow the region?
//
// This is the question that decides how the dock can grow taller than its plate
// during magnification. The dock window must become taller to let icons scale up,
// but the extra area is empty space that must not swallow clicks meant for the
// window underneath. T-005 already proved SetWindowRgn does NOT clip the system
// backdrop (Mica/accent acrylic paint over the region), so the region is only
// usable if it clips our own DComp content and moves the hit test with it.
//
// Modes:
//   --mode=paint    DComp surface + region, held for --hold-ms. Screenshot it.
//   --mode=victim   plain window underneath, logs the clicks it receives.
#include <windows.h>
#include <windowsx.h>

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kPaintClass[] = L"SpikeT010bPaint";
constexpr wchar_t kVictimClass[] = L"SpikeT010bVictim";

struct Options {
    std::wstring mode = L"paint";
    int x = 500;
    int y = 400;
    int width = 600;
    int height = 300;
    int band_top = 200;  // region starts here (window-local y)
    int band_bottom = 280;
    int hold_ms = 4000;
    bool region = false;
    bool click = false;
    std::wstring out;
};

Options g_options;
FILE* g_out = nullptr;
int g_victim_clicks = 0;
int g_paint_clicks = 0;
int g_paint_hits = 0;

void Emit(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    std::printf("%s\n", buffer);
    std::fflush(stdout);
    if (g_out != nullptr) {
        std::fprintf(g_out, "%s\n", buffer);
        std::fflush(g_out);
    }
}

long long NowMs() {
    static LARGE_INTEGER frequency{};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<long long>(counter.QuadPart * 1000 / frequency.QuadPart);
}

struct Surface {
    ComPtr<ID3D11Device> d3d;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> d2d_factory;
    ComPtr<ID2D1Device> d2d_device;
    ComPtr<IDCompositionDesktopDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual2> visual;
    ComPtr<IDCompositionSurface> surface;
    int width = 0;
    int height = 0;

    bool Init(HWND hwnd) {
        D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d, &got,
                                       nullptr);
        if (FAILED(hr)) {
            Emit("d3d=hardware_failed hr=0x%08lX, trying warp", static_cast<unsigned long>(hr));
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &d3d, &got, nullptr);
        }
        if (FAILED(hr)) {
            Emit("d3d=warp_failed hr=0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        hr = d3d.As(&dxgi);
        if (FAILED(hr)) return false;

        D2D1_FACTORY_OPTIONS options{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                               &options, reinterpret_cast<void**>(d2d_factory.GetAddressOf()));
        if (FAILED(hr)) return false;
        hr = d2d_factory->CreateDevice(dxgi.Get(), &d2d_device);
        if (FAILED(hr)) return false;

        // ID2D1Device, not IDXGIDevice: T-004 measured that passing the DXGI device
        // makes every call succeed until BeginDraw returns E_NOINTERFACE.
        hr = DCompositionCreateDevice2(d2d_device.Get(), __uuidof(IDCompositionDesktopDevice),
                                       reinterpret_cast<void**>(dcomp.GetAddressOf()));
        if (FAILED(hr)) return false;
        hr = dcomp->CreateTargetForHwnd(hwnd, TRUE, &target);
        if (FAILED(hr)) return false;
        hr = dcomp->CreateVisual(&visual);
        if (FAILED(hr)) return false;
        hr = visual->SetContent(surface.Get());  // null content, set after resize
        hr = target->SetRoot(visual.Get());
        return true;
    }

    bool Resize(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (w != width || h != height) {
            surface.Reset();
            HRESULT hr = dcomp->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                              DXGI_ALPHA_MODE_PREMULTIPLIED, &surface);
            if (FAILED(hr)) {
                Emit("surface=create_failed hr=0x%08lX", static_cast<unsigned long>(hr));
                return false;
            }
            visual->SetContent(surface.Get());
            width = w;
            height = h;
        }
        return true;
    }

    bool Paint(D2D1_COLOR_F color) {
        POINT offset{};
        ComPtr<ID2D1DeviceContext> dc;
        HRESULT hr = surface->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext),
                                        reinterpret_cast<void**>(dc.GetAddressOf()), &offset);
        if (FAILED(hr)) {
            Emit("begindraw=failed hr=0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        dc->SetDpi(96.0f, 96.0f);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                       static_cast<float>(offset.y)));
        ComPtr<ID2D1SolidColorBrush> brush;
        dc->CreateSolidColorBrush(color, &brush);
        const D2D1_RECT_F rect = D2D1::RectF(0.0f, 0.0f, static_cast<float>(width),
                                             static_cast<float>(height));
        dc->FillRectangle(rect, brush.Get());
        dc->SetTarget(nullptr);
        hr = surface->EndDraw();
        if (FAILED(hr)) {
            Emit("enddraw=failed hr=0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        hr = dcomp->Commit();
        if (FAILED(hr)) {
            Emit("commit=failed hr=0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        return true;
    }
};

Surface g_surface;

void PumpFor(int milliseconds) {
    const long long deadline = NowMs() + milliseconds;
    MSG msg{};
    while (NowMs() < deadline) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(5);
    }
}

LRESULT CALLBACK PaintProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST: {
            const LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
            if (g_paint_hits < 6) {
                ++g_paint_hits;
                POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                Emit("event=nchittest client_y=%d result=%ld", pt.y, static_cast<long>(hit));
            }
            return hit;
        }
        case WM_LBUTTONDOWN: {
            ++g_paint_clicks;
            Emit("event=paint_click y=%d", GET_Y_LPARAM(lp));
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT CALLBACK VictimProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_LBUTTONDOWN:
            ++g_victim_clicks;
            Emit("event=victim_click y=%d count=%d", GET_Y_LPARAM(lp), g_victim_clicks);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void SendClickAt(int screen_x, int screen_y) {
    SetCursorPos(screen_x, screen_y);
    Sleep(80);
    INPUT inputs[2]{};
    inputs[0].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].type = INPUT_MOUSE;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, inputs, sizeof(INPUT));
    Emit("sendinput=click at=%d,%d", screen_x, screen_y);
}

int RunVictim() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = VictimProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kVictimClass;
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST, kVictimClass, L"victim", WS_POPUP, g_options.x,
                                g_options.y, g_options.width, g_options.height, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        Emit("victim=create_failed err=%lu", GetLastError());
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    Emit("victim=ready rect=%d,%d %dx%d", g_options.x, g_options.y, g_options.width,
         g_options.height);
    PumpFor(g_options.hold_ms);
    Emit("victim=done clicks=%d", g_victim_clicks);
    return 0;
}

int RunPaint() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PaintProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPaintClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                                kPaintClass, L"paint", WS_POPUP, g_options.x, g_options.y,
                                g_options.width, g_options.height, nullptr, nullptr,
                                wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        Emit("paint=create_failed err=%lu", GetLastError());
        return 1;
    }
    if (!g_surface.Init(hwnd)) {
        Emit("paint=surface_init_failed");
        return 1;
    }
    if (!g_surface.Resize(g_options.width, g_options.height)) return 1;
    if (!g_surface.Paint(D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f))) {
        Emit("paint=fill_failed");
        return 1;
    }

    if (g_options.region) {
        HRGN rgn = CreateRectRgn(0, g_options.band_top, g_options.width,
                                 g_options.band_bottom);
        const int applied = SetWindowRgn(hwnd, rgn, TRUE);
        Emit("region=applied ret=%d band=%d..%d", applied, g_options.band_top,
             g_options.band_bottom);
    }

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    // Explicit z-order: two WS_EX_TOPMOST windows do not necessarily stack in
    // creation order (T-004 measured the reverse), so raise this one on purpose.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(hwnd);
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    Emit("paint=rect %ld,%ld %ldx%ld", rc.left, rc.top, rc.right - rc.left,
         rc.bottom - rc.top);
    g_surface.Paint(D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f));
    Emit("paint=ready rect=%d,%d %dx%d region=%d", g_options.x, g_options.y,
         g_options.width, g_options.height, g_options.region ? 1 : 0);

    if (g_options.click) {
        // y=20 is outside the band, y=(top+bottom)/2 is inside it.
        SendClickAt(g_options.x + g_options.width / 2, g_options.y + 20);
        PumpFor(250);
        SendClickAt(g_options.x + g_options.width / 2,
                    g_options.y + (g_options.band_top + g_options.band_bottom) / 2);
        PumpFor(250);
    }

    PumpFor(g_options.hold_ms);
    Emit("paint=summary clicks=%d", g_paint_clicks);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Without this the process is DPI-virtualised: at 192 DPI the window lands at
    // 2x the requested screen coordinates and the window-local band is scaled too,
    // so the synthesised clicks miss the window entirely and the test measures
    // nothing. Measured the hard way.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--mode=", 0) == 0) {
            g_options.mode = arg.substr(7);
        } else if (arg.rfind(L"--rect=", 0) == 0) {
            int x = 0, y = 0, w = 0, h = 0;
            if (swscanf_s(arg.c_str() + 7, L"%d,%d,%d,%d", &x, &y, &w, &h) == 4) {
                g_options.x = x;
                g_options.y = y;
                g_options.width = w;
                g_options.height = h;
            }
        } else if (arg.rfind(L"--band=", 0) == 0) {
            int top = 0, bottom = 0;
            if (swscanf_s(arg.c_str() + 7, L"%d..%d", &top, &bottom) == 2) {
                g_options.band_top = top;
                g_options.band_bottom = bottom;
            }
        } else if (arg.rfind(L"--hold-ms=", 0) == 0) {
            g_options.hold_ms = _wtoi(arg.c_str() + 10);
        } else if (arg == L"--region") {
            g_options.region = true;
        } else if (arg == L"--click") {
            g_options.click = true;
        } else if (arg.rfind(L"--out=", 0) == 0) {
            g_options.out = arg.substr(6);
        }
    }

    if (!g_options.out.empty()) _wfopen_s(&g_out, g_options.out.c_str(), L"wb");
    const int code = g_options.mode == L"victim" ? RunVictim() : RunPaint();
    if (g_out != nullptr) std::fclose(g_out);
    return code;
}
