// DockForge - borderless, per-monitor-DPI-aware top-level window wrapper.
#pragma once

#include "platform/win_common.h"

#include <functional>

namespace dockforge::platform {

struct WindowDesc {
    std::wstring title = L"DockForge";
    int width = 800;
    int height = 96;
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    bool topmost = true;
    bool click_through_outside = false;
    bool no_activate = false;
    // WS_EX_NOREDIRECTIONBITMAP: no DWM redirection surface, so GDI painting is
    // invisible and content must come from DirectComposition. Off by default so
    // plain GDI windows still render while the DComp path is being built.
    bool composition_surface = false;
};

// Owns a single HWND created from a shared DockForge window class.
// Subclasses receive raw messages through the message hook; returning true
// means "handled, do not run default handling".
class Window {
public:
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT&)>;

    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Registers the shared window class once per process.
    static bool RegisterWindowClass(HINSTANCE instance);

    bool Create(HINSTANCE instance, const WindowDesc& desc);
    void Destroy();

    HWND Handle() const { return hwnd_; }
    bool IsValid() const { return hwnd_ != nullptr; }

    // Message hook is invoked before default handling.
    void SetMessageHook(MessageHook hook) { hook_ = std::move(hook); }

    // DPI helpers.
    UINT Dpi() const;
    float Scale() const;

    // Applies rounded corners + dark-mode frame where supported.
    void ApplyRoundedCorners(int radius_px, bool dark);

    // Runs a standard message pump until WM_QUIT. Returns the quit code.
    static int RunMessageLoop();

    // Posts WM_QUIT to the calling thread.
    static void Quit(int code = 0);

    // Work area of the monitor that hosts the window.
    RECT MonitorWorkArea() const;

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    MessageHook hook_;
};

}  // namespace dockforge::platform
