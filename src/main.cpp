// DockForge entry point (T-001 skeleton).
#include "core/log.h"
#include "core/paths.h"
#include "platform/window.h"

#include <dwmapi.h>

namespace {

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

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    EnablePerMonitorDpi();

    // COM is required by every shell/DirectX surface DockForge touches.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }
    auto com_guard = dockforge::MakeGuard([] { CoUninitialize(); });

    using namespace dockforge;
    core::Logger::Instance().Init(L"app");
    DF_INFO("app", "DockForge %s starting", "0.1.0");
    DF_INFO("app", "config path = %s",
            core::WideToUtf8(core::ConfigPath()).c_str());

    if (!platform::Window::RegisterWindowClass(instance)) {
        DF_FATAL("app", "window class registration failed");
        return 2;
    }

    platform::WindowDesc desc;
    desc.title = L"DockForge";
    desc.width = 720;
    desc.height = 120;
    desc.topmost = true;
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

    // T-001 skeleton: prove the window lives, can be painted, and quits cleanly.
    // The real composition surface lands in T-004/T-005.
    window.SetMessageHook([](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& out) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps{};
                HDC dc = BeginPaint(hwnd, &ps);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                HBRUSH brush = CreateSolidBrush(RGB(24, 26, 32));
                FillRect(dc, &rc, brush);
                DeleteObject(brush);
                SetBkMode(dc, TRANSPARENT);
                SetTextColor(dc, RGB(236, 240, 248));
                const wchar_t* text = L"DockForge · skeleton window (Ctrl+Q to quit)";
                DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                EndPaint(hwnd, &ps);
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
    }

    const int code = platform::Window::RunMessageLoop();
    DF_INFO("app", "message loop exited with code %d", code);
    core::Logger::Instance().Shutdown();
    return code;
}
