#include "platform/preview_window.h"

#include "core/log.h"

#include <windowsx.h>
#include <cstdint>

namespace dockforge::platform {
namespace {

constexpr wchar_t kPreviewClass[] = L"DockForgePreviewWindow";

// Background of the popup: near-black so the slot gutters read as separation.
// DWM composites the thumbnails on top of this, so it only shows in the gaps.
constexpr COLORREF kBackground = RGB(16, 17, 20);
constexpr char kLogModule[] = "preview";

HBRUSH BackgroundBrush() {
    static HBRUSH brush = CreateSolidBrush(kBackground);
    return brush;
}

}  // namespace

PreviewWindow::~PreviewWindow() { Destroy(); }

bool PreviewWindow::RegisterWindowClass(HINSTANCE instance) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PreviewWindow::StaticWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // No class background: WM_ERASEBKGND is handled explicitly so painting is
    // fully under our control and never flickers through a brush.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPreviewClass;

    ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        DF_ERROR(kLogModule, "RegisterClassExW failed 0x%08lX", GetLastError());
        return false;
    }
    return true;
}

bool PreviewWindow::Create(HINSTANCE instance) {
    if (hwnd_) {
        return true;
    }
    if (!RegisterWindowClass(instance)) {
        return false;
    }

    // Created hidden at 1x1; Place() sets the real geometry. The HWND must exist
    // before any DwmRegisterThumbnail call, because DWM renders into this
    // window's redirection surface.
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kPreviewClass,
                            L"DockForgePreview", WS_POPUP, 0, 0, 1, 1, nullptr,
                            nullptr, instance, this);
    if (!hwnd_) {
        DF_ERROR(kLogModule, "CreateWindowExW failed 0x%08lX", GetLastError());
        return false;
    }
    width_px_ = 1;
    height_px_ = 1;
    return true;
}

void PreviewWindow::Destroy() {
    if (!hwnd_) {
        return;
    }
    // The owner must have released every DWM thumbnail already; a live
    // registration on a destroyed HWND is the classic shutdown hang.
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    width_px_ = 0;
    height_px_ = 0;
}

void PreviewWindow::Place(int left_px, int top_px, int width_px, int height_px) {
    if (!hwnd_) {
        return;
    }
    width_px_ = width_px;
    height_px_ = height_px;

    // TOPMOST keeps the preview above normal windows; NOACTIVATE keeps focus in
    // whatever the user was doing. SWP_SHOWWINDOW is what makes it visible.
    SetWindowPos(hwnd_, HWND_TOPMOST, left_px, top_px, width_px, height_px,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void PreviewWindow::Hide() {
    if (!hwnd_) {
        return;
    }
    ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK PreviewWindow::StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam,
                                              LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self = reinterpret_cast<PreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return self->WndProc(hwnd, msg, wparam, lparam);
}

LRESULT PreviewWindow::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_ERASEBKGND:
            // Suppress the class background so the WM_PAINT fill is the only
            // paint that ever lands.
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(dc, &rc, BackgroundBrush());
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
            // The preview is informational; a click inside it dismisses it.
            if (click_hook_) {
                click_hook_();
            }
            return 0;

        case WM_NCHITTEST:
            // Everything inside the popup belongs to the popup, including the
            // caption strip, so a click never reaches the window underneath.
            return HTCLIENT;

        case WM_DESTROY:
            hwnd_ = nullptr;
            width_px_ = 0;
            height_px_ = 0;
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}

}  // namespace dockforge::platform
