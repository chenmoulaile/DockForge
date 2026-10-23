// DockForge entry point.
//
// T-001 skeleton window, T-002 config wiring, T-003 frame-driven app loop,
// T-004 DirectComposition surface.
#include "core/app_loop.h"
#include "core/config.h"
#include "core/event_bus.h"
#include "core/frame_clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "platform/composition.h"
#include "platform/window.h"

#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>

#include <cmath>

namespace {

using namespace dockforge;

void EnablePerMonitorDpi() {
    // Windows 10 1703+ understands DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
    // The call fails harmlessly on older builds where the manifest already
    // declared system DPI awareness.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

bool EnableDarkTitleBar(HWND hwnd) {
    constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
    BOOL dark = TRUE;
    return SUCCEEDED(
        DwmSetWindowAttribute(hwnd, kDwmwaUseImmersiveDarkMode, &dark, sizeof(dark)));
}

// DOCKFORGE_DPI_OVERRIDE=<96|120|144|192> forces the layout DPI. The DPI matrix
// in COMPATIBILITY.md has to be provable on a single-DPI machine, and Windows'
// per-monitor DPI cannot be faked per-window otherwise. Test hook only.
void ApplyDpiOverrideFromEnvironment() {
    wchar_t buffer[16]{};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_DPI_OVERRIDE", buffer,
                                            ARRAYSIZE(buffer));
    if (n == 0 || n >= ARRAYSIZE(buffer)) return;
    const int value = _wtoi(buffer);
    if (value >= 72 && value <= 480) {
        platform::Window::SetDpiOverride(static_cast<UINT>(value));
    }
}

// Logical (DIP) content size of the skeleton window. T-008 replaces this with
// the real Dock layout engine; until then the box scales with the config so the
// C-01 wiring stays observable.
struct LogicalSize {
    float width;
    float height;
};

LogicalSize ComputeLogicalSize(const core::Config& config) {
    // Kept deliberately modest: the dev host runs a 192 DPI session whose
    // logical work area is only 720x480 DIP, so a bigger skeleton would hang off
    // the screen and make screenshot evidence unusable. T-008 derives the real
    // Dock width from the icon count instead.
    return LogicalSize{240.0f + static_cast<float>(config.dock.icon_size) * 3.0f,
                       static_cast<float>(config.dock.icon_size) + 56.0f};
}

// DirectWrite text format for the skeleton label. Created once per process.
IDWriteTextFormat* SharedLabelFormat() {
    static ComPtr<IDWriteFactory> factory = [] {
        ComPtr<IDWriteFactory> created;
        const HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                               __uuidof(IDWriteFactory),
                                               reinterpret_cast<IUnknown**>(created.GetAddressOf()));
        if (FAILED(hr)) {
            DF_ERROR("render", "DWriteCreateFactory failed: %s",
                     core::WideToUtf8(HResultText(hr)).c_str());
            return ComPtr<IDWriteFactory>{};
        }
        return created;
    }();

    static ComPtr<IDWriteTextFormat> format = [&] {
        ComPtr<IDWriteTextFormat> created;
        if (!factory) return ComPtr<IDWriteTextFormat>{};
        const HRESULT hr = factory->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-CN", &created);
        if (FAILED(hr)) {
            // Windows 10 ships "Segoe UI" only; the variable family is Win11+.
            factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      15.0f, L"zh-CN", &created);
        }
        return created;
    }();
    return format.Get();
}

// Draws the T-004 placeholder content: a rounded panel proving per-pixel alpha,
// a hairline border proving edge coverage, and a label proving DirectWrite.
void PaintDockContent(ID2D1DeviceContext& dc, const platform::CompositionSurface::PaintInfo& info,
                      const core::Config& config) {
    // Lay out against the surface size handed in, never against
    // dc.GetPixelSize(): that is the composition atlas, not this surface.
    const float wdip = info.width_dip;
    const float hdip = info.height_dip;
    if (wdip <= 1.0f || hdip <= 1.0f) return;

    ComPtr<ID2D1SolidColorBrush> fill;
    dc.CreateSolidColorBrush(D2D1::ColorF(0.094f, 0.102f, 0.125f, 0.82f), fill.GetAddressOf());

    ComPtr<ID2D1SolidColorBrush> border;
    dc.CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), border.GetAddressOf());

    const float radius = static_cast<float>(config.appearance.corner_radius);
    const D2D1_ROUNDED_RECT body{
        D2D1::RectF(0.5f, 0.5f, wdip - 0.5f, hdip - 0.5f), radius, radius};

    dc.FillRoundedRectangle(&body, fill.Get());
    dc.DrawRoundedRectangle(&body, border.Get(), 1.0f);

    IDWriteTextFormat* format = SharedLabelFormat();
    if (!format) return;

    ComPtr<ID2D1SolidColorBrush> text_brush;
    dc.CreateSolidColorBrush(D2D1::ColorF(0.925f, 0.941f, 0.973f, 1.0f), text_brush.GetAddressOf());

    wchar_t label[192];
    swprintf_s(label, L"DockForge \u00b7 icon %d px \u00b7 gap %d \u00b7 mag %.1fx \u00b7 %s",
               config.dock.icon_size, config.dock.gap,
               static_cast<double>(config.dock.magnification),
               core::ToString(config.dock.hover_falloff));
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    (dc.DrawText)(label, static_cast<UINT32>(wcslen(label)), format,
                 D2D1::RectF(0.0f, 0.0f, wdip, hdip), text_brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                 DWRITE_MEASURING_MODE_NATURAL);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    EnablePerMonitorDpi();

    // COM is required by every shell/DirectX surface DockForge touches.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    auto com_guard = MakeGuard([] { CoUninitialize(); });

    core::Logger::Instance().Init(L"app");
    DF_INFO("app", "DockForge %s starting", "0.1.0");
    DF_INFO("app", "config path = %s", core::WideToUtf8(core::ConfigPath()).c_str());
    ApplyDpiOverrideFromEnvironment();

    core::Config config = core::LoadConfig();
    DF_INFO("app", "config: icon_size=%d gap=%d mag=%.2f falloff=%s material=%s corner=%d",
            config.dock.icon_size, config.dock.gap,
            static_cast<double>(config.dock.magnification),
            core::ToString(config.dock.hover_falloff),
            core::ToString(config.appearance.material), config.appearance.corner_radius);

    if (!platform::Window::RegisterWindowClass(instance)) {
        DF_FATAL("app", "window class registration failed");
        return 2;
    }

    const LogicalSize logical = ComputeLogicalSize(config);

    platform::WindowDesc desc;
    desc.title = L"DockForge";
    // Window size is physical pixels; content is laid out in DIPs (ADR-0007).
    // The DPI is not known until the HWND exists, so start at 96 DPI and correct
    // after creation with an explicit resize.
    desc.width = static_cast<int>(std::lround(logical.width));
    desc.height = static_cast<int>(std::lround(logical.height));
    desc.topmost = true;
    // Content comes from DirectComposition only (ADR-0007).
    desc.composition_surface = true;
    // The skeleton window must accept keyboard focus so Ctrl+Q can be proven
    // here. The real Dock window flips this off in T-009 (it is click-driven
    // and must never steal focus).
    desc.no_activate = false;

    platform::Window window;
    if (!window.Create(instance, desc)) {
        DF_FATAL("app", "window creation failed");
        return 3;
    }
    EnableDarkTitleBar(window.Handle());

    platform::CompositionSurface surface;
    if (!surface.Initialize(window.Handle())) {
        DF_FATAL("app", "composition surface initialization failed");
        return 4;
    }

    // Resize to the DPI-correct physical size now that the monitor is known.
    auto resize_to_logical = [&window, &surface](LogicalSize size) {
        const float scale = window.Scale();
        const int w = static_cast<int>(std::lround(size.width * scale));
        const int h = static_cast<int>(std::lround(size.height * scale));
        RECT rc{};
        GetWindowRect(window.Handle(), &rc);
        SetWindowPos(window.Handle(), nullptr, 0, 0, w, h,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        surface.Resize(w, h);
    };
    resize_to_logical(logical);

    auto repaint = [&window, &surface, &config] {
        const float dpi = static_cast<float>(window.Dpi());
        if (!surface.Paint(dpi, [&config](ID2D1DeviceContext& dc,
                                          const platform::CompositionSurface::PaintInfo& info) {
                PaintDockContent(dc, info, config);
            })) {
            return false;
        }
        return surface.Commit();
    };

    core::EventBus bus;
    core::AppLoop loop;
    loop.SetTargetIntervalMs(16);  // 60 FPS target (PERFORMANCE.md)

    // T-003 wiring: the loop drives repaints only when something asked for one,
    // so an idle Dock stays at ~0% CPU. Later milestones publish layout/animation
    // events here instead of invalidating unconditionally.
    loop.SetTickHandler([hwnd = window.Handle()](double delta_ms) {
        // Animation work lands here in T-010. For now the tick only proves the
        // cadence: a repaint request is issued when a frame budget was missed,
        // which keeps idle CPU at zero while still exercising the path.
        if (delta_ms > 100.0) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    });
    loop.SetStatsHandler([](const core::FrameClock& clock) {
        DF_DEBUG("loop", "frames=%zu avg=%.2fms p50=%.2fms p99=%.2fms fps=%.1f",
                 clock.SampleCount(), clock.AverageMs(), clock.Percentile(50.0),
                 clock.Percentile(99.0), clock.AverageFps());
    });

    window.SetMessageHook([&loop, &repaint, &resize_to_logical, &surface, logical](
                              HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) {
        switch (msg) {
            case WM_PAINT: {
                // A NOREDIRECTIONBITMAP window has no GDI surface; BeginPaint is
                // only used to consume the update region.
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                repaint();
                EndPaint(hwnd, &ps);
                out = 0;
                return true;
            }
            case WM_DPICHANGED: {
                const auto* suggested = reinterpret_cast<const RECT*>(lp);
                if (suggested) {
                    SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                                 suggested->right - suggested->left,
                                 suggested->bottom - suggested->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                resize_to_logical(logical);
                InvalidateRect(hwnd, nullptr, FALSE);
                out = 0;
                return true;
            }
            case WM_SIZE: {
                surface.Resize(LOWORD(lp), HIWORD(lp));
                InvalidateRect(hwnd, nullptr, FALSE);
                out = 0;
                return true;
            }
            case WM_KEYDOWN:
                if (wp == 'Q' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    out = 0;
                    return true;
                }
                return false;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                loop.Quit(0);
                out = 0;
                return true;
            case WM_DESTROY:
                loop.Quit(0);
                out = 0;
                return true;
            default:
                return false;
        }
    });

    ShowWindow(window.Handle(), SW_SHOWNOACTIVATE);
    UpdateWindow(window.Handle());

    // Center the window on the primary monitor's work area.
    RECT work = window.MonitorWorkArea();
    if (work.right > work.left) {
        RECT rc{};
        GetWindowRect(window.Handle(), &rc);
        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const int x = work.left + ((work.right - work.left) - w) / 2;
        const int y = work.top + ((work.bottom - work.top) - h) / 2;
        SetWindowPos(window.Handle(), nullptr, x, y, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        GetWindowRect(window.Handle(), &rc);
        DF_INFO("app", "work=%ld,%ld,%ld,%ld target=%d,%d rect=%ld,%ld,%ld,%ld dpi=%u",
                work.left, work.top, work.right, work.bottom, x, y, rc.left, rc.top, rc.right,
                rc.bottom, window.Dpi());
    }
    repaint();

    const int code = loop.Run();
    DF_INFO("app", "app loop exited with code %d", code);
    surface.Shutdown();
    bus.Clear();
    core::Logger::Instance().Shutdown();
    return code;
}
