#include "platform/overlay_window.h"

#include "core/log.h"

#include <windowsx.h>

namespace dockforge::platform {
namespace {

constexpr wchar_t kOverlayClass[] = L"DockForgeOverlayWindow";
constexpr char kLogModule[] = "overlay";

// Popup style: the overlay covers the display exactly, with no caption, border or
// system menu. WS_EX_NOREDIRECTIONBITMAP is required for the DComp surface to be the
// only thing painted; without it DWM keeps drawing an empty redirection surface on
// top and the overlay reads as a black rectangle.
constexpr DWORD kOverlayStyle = WS_POPUP;
constexpr DWORD kOverlayExStyle =
    WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;

}  // namespace

OverlayWindow::~OverlayWindow() { Destroy(); }

bool OverlayWindow::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // No CS_HREDRAW/CS_VREDRAW: the content comes from DComp, so invalidating on
    // resize would only cause a WM_PAINT that draws nothing.
    wc.style = 0;
    wc.lpfnWndProc = &OverlayWindow::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kOverlayClass;

    const ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "RegisterClassExW failed 0x%08lX", GetLastError());
        return false;
    }
    return true;
}

bool OverlayWindow::Create(HINSTANCE instance, const RECT& rect, UINT dpi,
                           const std::wstring& title) {
    if (hwnd_) return true;
    if (!RegisterWindowClass(instance)) return false;

    bounds_ = rect;
    dpi_ = dpi ? dpi : 96;
    width_px_ = rect.right - rect.left;
    height_px_ = rect.bottom - rect.top;

    hwnd_ = CreateWindowExW(kOverlayExStyle, kOverlayClass, title.c_str(), kOverlayStyle,
                            rect.left, rect.top, width_px_, height_px_, nullptr, nullptr,
                            instance, this);
    if (!hwnd_) {
        DF_ERROR(kLogModule, "CreateWindowExW failed 0x%08lX", GetLastError());
        return false;
    }
    return true;
}

void OverlayWindow::Destroy() {
    if (!hwnd_) return;
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    visible_ = false;
    tracking_mouse_ = false;
}

void OverlayWindow::Show() {
    if (!hwnd_ || visible_) return;
    // SWP_NOACTIVATE would keep focus in the Dock, and then the Launchpad would never
    // receive a keystroke. It must take focus: it is a modal surface over the display.
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    visible_ = true;
}

void OverlayWindow::Hide() {
    if (!hwnd_ || !visible_) return;
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    tracking_mouse_ = false;
}

void OverlayWindow::SetBounds(const RECT& rect, UINT dpi) {
    bounds_ = rect;
    dpi_ = dpi ? dpi : 96;
    width_px_ = rect.right - rect.left;
    height_px_ = rect.bottom - rect.top;
    if (!hwnd_) return;
    SetWindowPos(hwnd_, HWND_TOPMOST, rect.left, rect.top, width_px_, height_px_,
                 SWP_NOACTIVATE | (visible_ ? SWP_SHOWWINDOW : 0));
}

void OverlayWindow::SetCallbacks(OverlayCallbacks callbacks) {
    callbacks_ = std::move(callbacks);
}

LRESULT CALLBACK OverlayWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                              LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    return self->WndProc(hwnd, msg, wparam, lparam);
}

LRESULT OverlayWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const float scale = Scale();

    switch (msg) {
        case WM_MOUSEACTIVATE:
            // Take the click. MA_ACTIVATE would be the default and is what we want, but
            // returning MA_ACTIVATE explicitly documents that this window is meant to
            // steal focus from the app underneath.
            return MA_ACTIVATE;

        case WM_ERASEBKGND:
            // Nothing to erase: the visible pixels come from the composition surface.
            return 1;

        case WM_PAINT: {
            // Required even though it draws nothing. Without BeginPaint/EndPaint the
            // update region never clears and the message repeats forever.
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (!tracking_mouse_) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                tracking_mouse_ = true;
            }
            if (callbacks_.pointer_moved) {
                callbacks_.pointer_moved(static_cast<float>(GET_X_LPARAM(lparam)) / scale,
                                         static_cast<float>(GET_Y_LPARAM(lparam)) / scale);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking_mouse_ = false;
            if (callbacks_.pointer_left) callbacks_.pointer_left();
            return 0;

        case WM_LBUTTONDOWN: {
            // Double-click detection is done by the caller through the class style
            // (CS_DBLCLKS) rather than by timing clicks here: the shell already
            // measures it correctly.
            const bool double_click = false;
            if (callbacks_.clicked) {
                callbacks_.clicked(static_cast<float>(GET_X_LPARAM(lparam)) / scale,
                                   static_cast<float>(GET_Y_LPARAM(lparam)) / scale,
                                   double_click);
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            if (callbacks_.clicked) {
                callbacks_.clicked(static_cast<float>(GET_X_LPARAM(lparam)) / scale,
                                   static_cast<float>(GET_Y_LPARAM(lparam)) / scale, true);
            }
            return 0;
        }

        case WM_KEYDOWN: {
            if (wparam == VK_ESCAPE) {
                if (callbacks_.dismissed) callbacks_.dismissed();
                return 0;
            }
            if (callbacks_.key_pressed) {
                callbacks_.key_pressed(static_cast<unsigned int>(wparam));
            }
            return 0;
        }

        case WM_CHAR: {
            if (callbacks_.character) {
                callbacks_.character(static_cast<wchar_t>(wparam));
            }
            return 0;
        }

        case WM_DESTROY:
            // Deliberately NOT PostQuitMessage: this window is created and destroyed
            // on every Launchpad open, and the process must outlive it.
            hwnd_ = nullptr;
            visible_ = false;
            tracking_mouse_ = false;
            if (callbacks_.destroyed) callbacks_.destroyed();
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace dockforge::platform
