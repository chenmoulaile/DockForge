#include "catch_amalgamated.hpp"

#include <windows.h>
#include <shellscalingapi.h>

#include <string>

#include "platform/display.h"

using dockforge::platform::ComputeDockPlacement;
using dockforge::platform::DisplayInfo;
using dockforge::platform::DockEdge;
using dockforge::platform::DipToPixels;
using dockforge::platform::PixelsToDip;

namespace {

// A synthetic display so placement math is testable without touching the real
// monitor configuration. Numbers are physical pixels, as everywhere else.
DisplayInfo MakeDisplay(int left, int top, int width, int height, UINT dpi,
                        int taskbar = 0) {
    DisplayInfo display;
    display.monitor = RECT{left, top, left + width, top + height};
    display.work = RECT{left, top, left + width, top + height - taskbar};
    display.effective_dpi = dpi;
    display.raw_dpi = dpi;
    return display;
}

}  // namespace

TEST_CASE("DipToPixels and PixelsToDip round-trip at every supported DPI") {
    // 96/120/144/168/192/240/288 cover 100..300% in the steps Windows offers.
    for (UINT dpi : {96u, 120u, 144u, 168u, 192u, 240u, 288u}) {
        INFO("dpi=" << dpi);
        CHECK(DipToPixels(0.0f, dpi) == 0);
        CHECK(DipToPixels(96.0f, dpi) == static_cast<int>(dpi));
        CHECK(PixelsToDip(static_cast<int>(dpi), dpi) == Catch::Approx(96.0f).margin(0.001f));
    }
    // Zero DPI must not divide by zero; it is treated as 96.
    CHECK(DipToPixels(96.0f, 0) == 96);
    CHECK(PixelsToDip(96, 0) == Catch::Approx(96.0f).margin(0.001f));
}

TEST_CASE("DipToPixels rounds to nearest, not truncating") {
    // 22.5 DIP at 144 DPI = 33.75 px -> 34, not 33.
    CHECK(DipToPixels(22.5f, 144) == 34);
    CHECK(DipToPixels(1.0f, 96) == 1);
    CHECK(DipToPixels(1.4f, 96) == 1);
    CHECK(DipToPixels(1.6f, 96) == 2);
}

TEST_CASE("Bottom dock is centered horizontally and inset by the margin") {
    const DisplayInfo display = MakeDisplay(0, 0, 1920, 1080, 96, /*taskbar=*/48);
    const auto placement = ComputeDockPlacement(display, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);

    CHECK(placement.dpi == 96);
    CHECK(placement.width_px == 600);
    CHECK(placement.height_px == 96);
    CHECK(placement.rect.left == (1920 - 600) / 2);
    CHECK(placement.rect.right == placement.rect.left + 600);
    // Work area bottom is 1080-48 = 1032; margin 8 -> bottom edge at 1024.
    CHECK(placement.rect.bottom == 1024);
    CHECK(placement.rect.top == 1024 - 96);
    CHECK_FALSE(placement.clamped);
}

TEST_CASE("Bottom dock respects the taskbar instead of covering it") {
    const DisplayInfo with_taskbar = MakeDisplay(0, 0, 1920, 1080, 96, 48);
    const DisplayInfo without = MakeDisplay(0, 0, 1920, 1080, 96, 0);
    const auto a = ComputeDockPlacement(with_taskbar, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);
    const auto b = ComputeDockPlacement(without, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);
    CHECK(a.rect.bottom == b.rect.bottom - 48);
    CHECK(a.rect.bottom < with_taskbar.work.bottom);
}

TEST_CASE("Left and right docks hug their edge and center vertically") {
    const DisplayInfo display = MakeDisplay(0, 0, 1920, 1080, 96, 48);
    const auto left = ComputeDockPlacement(display, DockEdge::Left, 96.0f, 600.0f, 8.0f);
    CHECK(left.rect.left == 8);
    CHECK(left.rect.right == 104);
    CHECK(left.rect.top == (1032 - 600) / 2);

    const auto right = ComputeDockPlacement(display, DockEdge::Right, 96.0f, 600.0f, 8.0f);
    CHECK(right.rect.right == 1920 - 8);
    CHECK(right.rect.left == right.rect.right - 96);
    CHECK(right.rect.top == left.rect.top);
}

TEST_CASE("Placement scales with the display DPI") {
    const DisplayInfo at_96 = MakeDisplay(0, 0, 1920, 1080, 96, 48);
    const DisplayInfo at_192 = MakeDisplay(0, 0, 3840, 2160, 192, 96);

    const auto a = ComputeDockPlacement(at_96, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);
    const auto b = ComputeDockPlacement(at_192, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);

    CHECK(b.width_px == a.width_px * 2);
    CHECK(b.height_px == a.height_px * 2);
    CHECK(b.scale == Catch::Approx(2.0f));
    // The DIP inset is the same distance from the edge in DIP terms.
    const int a_inset = at_96.work.bottom - a.rect.bottom;
    const int b_inset = at_192.work.bottom - b.rect.bottom;
    CHECK(b_inset == a_inset * 2);
}

TEST_CASE("A dock wider than the work area is clamped, not clipped") {
    const DisplayInfo display = MakeDisplay(0, 0, 800, 600, 96, 40);
    const auto placement = ComputeDockPlacement(display, DockEdge::Bottom, 2000.0f, 96.0f, 8.0f);

    CHECK(placement.clamped);
    CHECK(placement.width_px == 800 - 16);
    CHECK(placement.rect.left == 8);
    CHECK(placement.rect.right == 800 - 8);
    CHECK(placement.rect.left >= display.work.left);
    CHECK(placement.rect.right <= display.work.right);
}

TEST_CASE("A dock taller than the work area is clamped") {
    const DisplayInfo display = MakeDisplay(0, 0, 800, 300, 96, 40);
    const auto placement = ComputeDockPlacement(display, DockEdge::Left, 96.0f, 900.0f, 8.0f);
    CHECK(placement.clamped);
    CHECK(placement.height_px == 260 - 16);
    CHECK(placement.rect.top >= display.work.top);
    CHECK(placement.rect.bottom <= display.work.bottom);
}

TEST_CASE("Placement works on a secondary monitor with a negative origin") {
    // A monitor placed to the left of the primary has negative x, which is the
    // classic place naive "assume origin 0,0" math breaks.
    const DisplayInfo display = MakeDisplay(-1920, -200, 1920, 1080, 96, 48);
    const auto placement = ComputeDockPlacement(display, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);

    CHECK(placement.rect.left == -1920 + (1920 - 600) / 2);
    CHECK(placement.rect.right == placement.rect.left + 600);
    CHECK(placement.rect.bottom == -200 + 1080 - 48 - 8);
    CHECK(placement.rect.left >= display.work.left);
    CHECK(placement.rect.right <= display.work.right);
}

TEST_CASE("Mixed-DPI dual monitor: each display places at its own scale") {
    // 100% primary plus a 150% secondary, the COMPATIBILITY.md row that matters.
    const DisplayInfo primary = MakeDisplay(0, 0, 1920, 1080, 96, 48);
    const DisplayInfo secondary = MakeDisplay(1920, 0, 2560, 1440, 144, 60);

    const auto a = ComputeDockPlacement(primary, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);
    const auto b = ComputeDockPlacement(secondary, DockEdge::Bottom, 600.0f, 96.0f, 8.0f);

    CHECK(a.width_px == 600);
    CHECK(b.width_px == 900);  // 600 DIP at 150%
    CHECK(b.rect.left == 1920 + (2560 - 900) / 2);
    CHECK(b.rect.bottom == 1440 - 60 - 12);  // 8 DIP margin = 12 px at 150%
}

TEST_CASE("Degenerate work area falls back to the monitor rect") {
    DisplayInfo display;
    display.monitor = RECT{0, 0, 1000, 700};
    display.work = RECT{0, 0, 0, 0};  // enumeration gave nothing usable
    display.effective_dpi = 96;
    const auto placement = ComputeDockPlacement(display, DockEdge::Bottom, 400.0f, 80.0f, 4.0f);
    CHECK(placement.width_px == 400);
    CHECK(placement.rect.bottom == 700 - 4);
    CHECK(placement.rect.top == 700 - 4 - 80);
}

TEST_CASE("Enumerating displays on this host yields at least the primary") {
    // Environment-dependent by nature, but "no monitors at all" is never correct
    // and would silently make every placement test meaningless.
    const auto displays = dockforge::platform::EnumerateDisplays();
    REQUIRE_FALSE(displays.empty());
    CHECK(displays.front().primary);
    for (const auto& display : displays) {
        CHECK(display.effective_dpi >= 48);
        CHECK(display.effective_dpi <= 768);
        CHECK(display.monitor.right > display.monitor.left);
        CHECK(display.monitor.bottom > display.monitor.top);
        CHECK(display.work.right >= display.work.left);
    }
}

TEST_CASE("DisplayForWindow resolves the monitor hosting a real window") {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DockForgeDisplayTestWindow";
    RegisterClassExW(&wc);
    const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"t", WS_POPUP, 100, 100, 200, 100,
                                      nullptr, nullptr, wc.hInstance, nullptr);
    REQUIRE(hwnd != nullptr);

    DisplayInfo display;
    CHECK(dockforge::platform::DisplayForWindow(hwnd, display));
    CHECK(display.monitor.right > display.monitor.left);
    CHECK(display.effective_dpi >= 48);

    // A null window cannot resolve, and must not crash.
    DisplayInfo none;
    CHECK_FALSE(dockforge::platform::DisplayForWindow(nullptr, none));

    DestroyWindow(hwnd);
}

TEST_CASE("DisplayForPoint resolves a point inside the virtual screen") {
    DisplayInfo display;
    CHECK(dockforge::platform::DisplayForPoint(POINT{0, 0}, display));
    CHECK(display.effective_dpi >= 48);

    DisplayInfo primary;
    REQUIRE(dockforge::platform::PrimaryDisplay(primary));
    CHECK(primary.primary);
}
