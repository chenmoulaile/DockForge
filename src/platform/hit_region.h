// DockForge - clickable region for a dock window (T-010).
//
// Why a window region and not WM_NCHITTEST: the dock window is always tall enough
// for a fully magnified icon, so most of its area is transparent dead space above
// the plate. That space must not swallow clicks meant for the window underneath.
// Returning HTTRANSPARENT does not work across processes - measured in
// spikes/t010_hover (README, question 2): the click was swallowed by the overlay and
// the window underneath received nothing. SetWindowRgn does work: it clips our own
// DirectComposition content AND moves hit-testing with it (question 3).
#pragma once

#include "platform/win_common.h"

namespace dockforge::platform {

// Builds the clickable region for the dock in window-local pixels.
//
// `plate` is the rounded background; `row` is the area the icon row currently
// covers, which extends above the plate when icons magnify. When `include_row` is
// false only the plate is clickable, which is the idle state.
//
// Returns a region the caller owns (DeleteObject), or nullptr on failure.
HRGN CreateDockRegion(const RECT& plate, int radius_px, const RECT& row, bool include_row);

// Applies `region` to `hwnd` and takes ownership, matching SetWindowRgn's contract.
// A null region clears any existing one.
bool ApplyWindowRegion(HWND hwnd, HRGN region);

}  // namespace dockforge::platform
