// DockForge - full-screen overlay window (ADR-0021: one per display).
//
// Why this is not platform::Window:
//
//   * platform::Window shares one window class whose WndProc calls PostQuitMessage on
//     WM_DESTROY. That is correct for the single Dock window and fatal for an overlay
//     that is created and destroyed on every open - closing the Launchpad would end
//     the process.
//   * The overlay must swallow every input message it receives (it is modal over the
//     display) and report clicks back to its owner, which is a different policy from
//     the Dock's hook chain.
//   * The overlay owns a composition surface, so it needs WS_EX_NOREDIRECTIONBITMAP,
//     and it must NOT be WS_EX_TOOLWINDOW: a Launchpad that vanishes from Alt+Tab is
//     fine, but WS_EX_TOOLWINDOW also suppresses the window from some shell
//     interactions that the settings window will need later. Keeping a distinct class
//     lets each surface choose.
//
// One instance covers one display. Mixed-DPI setups get one overlay per monitor with
// its own composition tree, because a single window cannot carry two DPI values and a
// DComp visual tree is bound to one HWND (ADR-0021 decision 1, measured in
// spikes/t004_dcomp).
#pragma once

#include <functional>
#include <string>

#include "platform/win_common.h"

namespace dockforge::platform {

// Messages the overlay turns into callbacks. Everything else is consumed.
struct OverlayCallbacks {
    // Pointer moved to (x, y) in DIP relative to the overlay's client area.
    std::function<void(float x_dip, float y_dip)> pointer_moved;
    std::function<void()> pointer_left;
    // Left button went down / came up, in DIP. The press/release split is what makes
    // drag-to-create expressible (T-042): a press that moves onto another cell before
    // its release is a drag, a press released where it started is a click. The owner
    // owns that distinction; the overlay only reports the two edges.
    std::function<void(float x_dip, float y_dip)> pressed;
    std::function<void(float x_dip, float y_dip)> released;
    // A key was pressed. `vk` is a virtual-key code; text arrives separately through
    // `character` (WM_CHAR), which also carries a second copy of keys WM_KEYDOWN
    // already reported - the owner filters the overlap.
    std::function<void(unsigned int vk)> key_pressed;
    // A character was typed, for the search field.
    std::function<void(wchar_t ch)> character;
    // The user asked to dismiss: Escape, or a click outside the grid when the owner
    // says the click missed. The owner decides what "dismiss" means.
    std::function<void()> dismissed;
    // The overlay was destroyed by the system (display change, session end). The owner
    // must drop its surface.
    std::function<void()> destroyed;
};

class OverlayWindow {
  public:
    OverlayWindow() = default;
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // Registers the class once per process. Safe to call repeatedly.
    static bool RegisterWindowClass(HINSTANCE instance);

    // Creates the HWND covering `rect` (physical pixels, virtual-screen coordinates).
    // The window starts hidden; call Show() to present it. `dpi` is the display's
    // effective DPI and is what the owner must use for layout - the overlay reports it
    // back through Dpi() rather than letting callers ask GetDpiForWindow, which
    // returns the value for whichever monitor the window currently straddles.
    bool Create(HINSTANCE instance, const RECT& rect, UINT dpi, const std::wstring& title);

    void Destroy();
    bool IsValid() const { return hwnd_ != nullptr; }
    HWND Handle() const { return hwnd_; }

    void Show();
    void Hide();
    bool Visible() const { return visible_; }

    // Re-positions after a display change. Does not show or hide.
    void SetBounds(const RECT& rect, UINT dpi);

    void SetCallbacks(OverlayCallbacks callbacks);

    UINT Dpi() const { return dpi_ ? dpi_ : 96; }
    float Scale() const { return static_cast<float>(Dpi()) / 96.0f; }
    int WidthPx() const { return width_px_; }
    int HeightPx() const { return height_px_; }
    float WidthDip() const { return static_cast<float>(width_px_) / Scale(); }
    float HeightDip() const { return static_cast<float>(height_px_) / Scale(); }

    // Window rectangle in physical pixels. Cached from the last SetBounds/Create so a
    // caller can place a child surface without a GetWindowRect round trip.
    const RECT& Bounds() const { return bounds_; }

  private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    HWND hwnd_ = nullptr;
    RECT bounds_{};
    UINT dpi_ = 96;
    int width_px_ = 0;
    int height_px_ = 0;
    bool visible_ = false;
    bool tracking_mouse_ = false;
    OverlayCallbacks callbacks_;
};

}  // namespace dockforge::platform
