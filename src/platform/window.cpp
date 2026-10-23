// DockForge - borderless top-level window implementation.
#include "platform/window.h"

#include "core/log.h"
#include "core/paths.h"

namespace dockforge::platform {
namespace {

constexpr wchar_t kWindowClass[] = L"DockForgeHostWindow";

// DWMWA_USE_IMMERSIVE_DARK_MODE is 20 on 20H1+, 19 on earlier builds.
constexpr DWORD kDwmwaUseImmersiveDarkModeLegacy = 19;
constexpr DWORD kDwmwaUseImmersiveDarkMode = 20;
// DWMWA_WINDOW_CORNER_PREFERENCE = 33 (Windows 11 22000+)
constexpr DWORD kDwmwaWindowCornerPreference = 33;
constexpr int kDwmWindowCornerRound = 2;

}  // namespace

bool Window::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // fully self-drawn
    wc.lpszClassName = kWindowClass;

    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0) {
        const DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            DF_ERROR("platform", "RegisterClassExW failed: %lu", err);
            return false;
        }
    }
    return true;
}

Window::~Window() { Destroy(); }

bool Window::Create(HINSTANCE instance, const WindowDesc& desc) {
    if (hwnd_) return true;

    DWORD ex_style = WS_EX_TOOLWINDOW;
    if (desc.composition_surface) ex_style |= WS_EX_NOREDIRECTIONBITMAP;
    if (desc.topmost) ex_style |= WS_EX_TOPMOST;
    if (desc.click_through_outside) ex_style |= WS_EX_LAYERED;
    if (desc.no_activate) ex_style |= WS_EX_NOACTIVATE;

    const DWORD style = WS_POPUP;

    hwnd_ = CreateWindowExW(ex_style, kWindowClass, desc.title.c_str(), style,
                            desc.x, desc.y, desc.width, desc.height,
                            nullptr, nullptr, instance, this);
    if (!hwnd_) {
        DF_ERROR("platform", "CreateWindowExW failed: %lu", GetLastError());
        return false;
    }

    DF_INFO("platform", "window created hwnd=%p dpi=%u", static_cast<void*>(hwnd_),
            GetDpiForWindow(hwnd_));
    return true;
}

void Window::Destroy() {
    if (!hwnd_) return;
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

UINT Window::Dpi() const {
    if (!hwnd_) return 96;
    const UINT dpi = GetDpiForWindow(hwnd_);
    return dpi ? dpi : 96;
}

float Window::Scale() const { return static_cast<float>(Dpi()) / 96.0f; }

void Window::ApplyRoundedCorners(int radius_px, bool dark) {
    if (!hwnd_) return;

    // Corner preference: DWMWCP_ROUND. Ignored on Windows 10 (returns E_INVALIDARG).
    int corner = radius_px > 0 ? kDwmWindowCornerRound : 1 /*DWMWCP_DONOTROUND*/;
    HRESULT hr = DwmSetWindowAttribute(hwnd_, kDwmwaWindowCornerPreference, &corner,
                                       sizeof(corner));
    if (FAILED(hr)) {
        const std::string hr_text =
            core::WideToUtf8(HResultText(hr));
        DF_DEBUG("platform", "corner preference unsupported (hr=%s)", hr_text.c_str());
    }

    BOOL use_dark = dark ? TRUE : FALSE;
    hr = DwmSetWindowAttribute(hwnd_, kDwmwaUseImmersiveDarkMode, &use_dark,
                               sizeof(use_dark));
    if (FAILED(hr)) {
        DwmSetWindowAttribute(hwnd_, kDwmwaUseImmersiveDarkModeLegacy, &use_dark,
                              sizeof(use_dark));
    }
}

RECT Window::MonitorWorkArea() const {
    RECT rc{};
    if (!hwnd_) return rc;
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) return info.rcWork;
    return rc;
}

LRESULT CALLBACK Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self) self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Window::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (hook_) {
        LRESULT result = 0;
        if (hook_(hwnd, msg, wp, lp, result)) return result;
    }

    switch (msg) {
        case WM_DESTROY:
            hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Window::RunMessageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

void Window::Quit(int code) { PostQuitMessage(code); }

}  // namespace dockforge::platform
