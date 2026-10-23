// spikes/t005_material/capture.cpp - backdrop capture spike (T-005, part 2).
//
// Why: the material spike measured that Mica and the accent-policy acrylic both
// fill the ENTIRE window rectangle and ignore the per-pixel alpha of a DComp
// surface, and that SetWindowRgn does not clip them. A Dock with a 22px custom
// corner radius therefore cannot use them. The frosted plate has to be drawn by
// us, which needs a trustworthy source for "what is behind this window".
//
// This spike answers, with pixel values:
//   1. Does BitBlt(GetDC(NULL)) include our own WS_EX_NOREDIRECTIONBITMAP DComp
//      window? (If yes, blurring that capture feeds our own output back in.)
//   2. Does SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) remove it from the
//      BitBlt capture while keeping it on screen?
//   3. Does PrintWindow(Progman, PW_RENDERFULLCONTENT) yield the wallpaper alone,
//      i.e. a stable capture with no window contamination at all?
//
// Ground truth: a checkerboard window with a KNOWN colour sits behind the dock
// panel. Reading that colour back proves the capture really came from the screen
// rather than from an empty buffer.
#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kDockClass[] = L"DockForgeSpikeCapDock";
constexpr wchar_t kBackdropClass[] = L"DockForgeSpikeCapBackdrop";
constexpr int kDockX = 300;
constexpr int kDockY = 300;
constexpr int kDockW = 320;
constexpr int kDockH = 120;

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

LRESULT CALLBACK BackdropProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(0, 200, 0));
        FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
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

bool MakeBackdrop(HINSTANCE inst, State& s) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = inst;
    wc.lpszClassName = kBackdropClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    s.backdrop = CreateWindowExW(WS_EX_TOOLWINDOW, kBackdropClass, L"cap-backdrop", WS_POPUP,
                                 kDockX - 30, kDockY - 30, kDockW + 60, kDockH + 60, nullptr,
                                 nullptr, inst, nullptr);
    if (!s.backdrop) return false;
    ShowWindow(s.backdrop, SW_SHOWNOACTIVATE);
    UpdateWindow(s.backdrop);
    return true;
}

bool MakeDock(HINSTANCE inst, State& s) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DockProc;
    wc.hInstance = inst;
    wc.lpszClassName = kDockClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    s.dock = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                             kDockClass, L"cap-dock", WS_POPUP, kDockX, kDockY, kDockW, kDockH,
                             nullptr, nullptr, inst, nullptr);
    return s.dock != nullptr;
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
                                 reinterpret_cast<void**>(s.d2d_factory.GetAddressOf()))))
        return false;
    if (FAILED(s.d2d_factory->CreateDevice(s.dxgi.Get(), &s.d2d_device))) return false;
    if (FAILED(DCompositionCreateDevice2(s.d2d_device.Get(), IID_PPV_ARGS(&s.dcomp))))
        return false;
    if (FAILED(s.dcomp->CreateTargetForHwnd(s.dock, TRUE, &s.target))) return false;
    if (FAILED(s.dcomp->CreateVisual(&s.visual))) return false;
    if (FAILED(s.dcomp->CreateSurface(kDockW, kDockH, DXGI_FORMAT_B8G8R8A8_UNORM,
                                      DXGI_ALPHA_MODE_PREMULTIPLIED, &s.surface)))
        return false;
    if (FAILED(s.visual->SetContent(s.surface.Get()))) return false;
    if (FAILED(s.target->SetRoot(s.visual.Get()))) return false;
    return true;
}

// Opaque magenta plate: unmistakable in a capture if the dock leaks in.
void PaintMagenta(State& s) {
    ComPtr<ID2D1DeviceContext> dc;
    POINT offset{};
    if (FAILED(s.surface->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &offset))) return;
    dc->SetDpi(96.0f, 96.0f);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(static_cast<float>(offset.x),
                                                   static_cast<float>(offset.y)));
    ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.0f, 1.0f, 1.0f), brush.GetAddressOf());
    dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, static_cast<float>(kDockW),
                                  static_cast<float>(kDockH)),
                      brush.Get());
    dc->SetTarget(nullptr);
    s.surface->EndDraw();
    s.dcomp->Commit();
}

struct Rgb {
    int r, g, b;
};

Rgb ReadAt(HDC src, int x, int y) {
    COLORREF c = GetPixel(src, x, y);
    if (c == CLR_INVALID) return {-1, -1, -1};
    return {GetRValue(c), GetGValue(c), GetBValue(c)};
}

void PrintRgb(const char* label, Rgb c) {
    std::printf("  %-28s rgb=(%3d,%3d,%3d)\n", label, c.r, c.g, c.b);
}

Rgb ReadBitmap(HBITMAP bmp, int x, int y) {
    BITMAP info{};
    GetObjectW(bmp, sizeof(info), &info);
    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(bih);
    bih.biWidth = info.bmWidth;
    bih.biHeight = -info.bmHeight;  // top-down
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bih), DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dib) return {-1, -1, -1};
    HDC dst = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dst, dib);
    HDC src = CreateCompatibleDC(nullptr);
    HGDIOBJ old2 = SelectObject(src, bmp);
    BitBlt(dst, 0, 0, info.bmWidth, info.bmHeight, src, 0, 0, SRCCOPY);
    SelectObject(src, old2);
    DeleteDC(src);

    Rgb out{-1, -1, -1};
    if (bits) {
        const auto* px = static_cast<const std::uint32_t*>(bits);
        const std::uint32_t v = px[y * info.bmWidth + x];
        out = {static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF),
               static_cast<int>((v >> 16) & 0xFF)};
    }
    SelectObject(dst, old);
    DeleteDC(dst);
    DeleteObject(dib);
    return out;
}

// BitBlt the screen region under the dock into a DIB and read three points.
// Samples: dock centre (would be magenta if our window leaks in), and a point just
// outside the dock (must be the green backdrop either way).
void CaptureScreenRegion(const char* label, int w, int h) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, kDockX, kDockY, SRCCOPY);
    std::printf("%s BitBlt=%s\n", label, ok ? "OK" : "FAIL");
    const Rgb center = ReadAt(mem, w / 2, h / 2);
    const Rgb outside = ReadAt(mem, 8, 8);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    PrintRgb("dock centre", center);
    PrintRgb("outside dock (corner)", outside);
}

void CaptureProgman() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    std::printf("PrintWindow(Progman) handle=%p\n", static_cast<void*>(progman));
    if (!progman) return;
    RECT rc{};
    GetWindowRect(progman, &rc);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    // PW_RENDERFULLCONTENT = 0x00000002, required for DWM-composited windows.
    const BOOL ok = PrintWindow(progman, mem, 0x00000002);
    std::printf("  PrintWindow size=%dx%d ok=%s\n", w, h, ok ? "OK" : "FAIL");
    const int sx = kDockX - rc.left + kDockW / 2;
    const int sy = kDockY - rc.top + kDockH / 2;
    PrintRgb("progman at dock centre", ReadBitmap(bmp, sx, sy));
    PrintRgb("progman far corner", ReadBitmap(bmp, w - 10, h - 10));
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool exclude = false;
    bool capture_only = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--exclude-from-capture") exclude = true;
        if (arg == L"--capture-only") capture_only = true;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    State s;
    if (!MakeBackdrop(inst, s)) return 1;
    if (!MakeDock(inst, s)) return 1;

    if (exclude) {
        const BOOL ok = SetWindowDisplayAffinity(s.dock, WDA_EXCLUDEFROMCAPTURE);
        std::printf("SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) = %s (err=%lu)\n",
                    ok ? "OK" : "FAIL", static_cast<unsigned long>(GetLastError()));
    }

    if (!BuildComposition(s)) {
        std::printf("composition stack failed\n");
        return 2;
    }
    ShowWindow(s.dock, SW_SHOWNOACTIVATE);
    PaintMagenta(s);
    // Give the compositor a frame to present before capturing.
    Sleep(500);

    if (!capture_only) {
        std::printf("\n--- screen BitBlt (dock visible, opaque magenta) ---\n");
        CaptureScreenRegion("screen", kDockW, kDockH);
    }
    std::printf("\n--- PrintWindow(Progman) ---\n");
    CaptureProgman();

    Sleep(400);
    return 0;
}
