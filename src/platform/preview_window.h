// DockForge - window thumbnail preview popup (T-015).
//
// Contract: CONTRACTS.md C-06 puts thumbnails in src/shell and window classes in
// src/platform. This module is the window half: it owns one HWND, its own window
// class and its own WndProc, and knows nothing about dock tiles.
#pragma once

#include "platform/win_common.h"

#include <functional>

namespace dockforge::platform {

// A plain top-level popup that hosts DWM thumbnails.
//
// It cannot reuse platform::Window, for two reasons that were both measured:
//   * that class shares one window class whose WndProc posts WM_QUIT on
//     WM_DESTROY, so a second Window in the same process would end the dock's
//     message loop when the popup closed;
//   * its message hook belongs to the dock and would have to be told about every
//     popup message.
// A popup that dies must therefore be its own class with its own proc.
//
// It is deliberately NOT WS_EX_NOREDIRECTIONBITMAP: DWM composites thumbnails on
// top of the destination window's redirection surface, so the GDI background
// painted here has to stay visible underneath them (spikes/t015_thumb conclusion
// 1: a GDI background under the thumbnails renders normally).
class PreviewWindow {
public:
    using ClickHook = std::function<void()>;

    PreviewWindow() = default;
    ~PreviewWindow();

    PreviewWindow(const PreviewWindow&) = delete;
    PreviewWindow& operator=(const PreviewWindow&) = delete;

    // Registers the popup window class once per process.
    static bool RegisterWindowClass(HINSTANCE instance);

    // Creates the window hidden at 1x1. Sizing happens in Place(), so the
    // client size and the DWM slot rectangles are always derived from one call.
    bool Create(HINSTANCE instance);
    void Destroy();

    bool IsValid() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }

    // Moves the popup to a virtual-screen rectangle in PHYSICAL pixels and shows
    // it. Called by the owner whenever the hovered tile changes.
    void Place(int left_px, int top_px, int width_px, int height_px);

    // Hides the popup. The HWND survives so the next popup does not pay for
    // window creation; DWM registration is the owner's job to release first.
    void Hide();

    void SetClickHook(ClickHook hook) { click_hook_ = std::move(hook); }

    int ClientWidthPx() const { return width_px_; }
    int ClientHeightPx() const { return height_px_; }

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    ClickHook click_hook_;
    int width_px_ = 0;
    int height_px_ = 0;
};

}  // namespace dockforge::platform
