// Work-area reservation for the Finder Bar (T-302b).
//
// Why this exists. The user asked for a bar that is permanently pinned. A pinned
// topmost bar covers the top 28dip of every maximized window, which is unacceptable,
// so the bar needs the shell to reserve that strip - exactly what the taskbar does
// for itself.
//
// Two mechanisms were tested (spikes/T-302_tray/appbar_spike.cpp):
//
//   SPI_SETWORKAREA        Q8 in spikes/T-301_finderbar_menu: returns success,
//                          changes nothing. Explorer rewrites the work area
//                          immediately. Dead end.
//
//   SHAppBarMessage        Q10..Q13: ABM_NEW -> 1, ABM_SETPOS -> 1 and the work
//                          area really moved from 0,0,2880,1824 to 0,56,2880,1824;
//                          a maximized window's top moved from -13 to 43, exactly
//                          the reserved 56px; ABM_REMOVE restored the original
//                          work area byte for byte. This is the taskbar's own
//                          mechanism and it works.
//
// So the bar can be pinned by default with no covering problem. The reservation is
// still an explicit, reversible system change, which is why every step here has a
// matching undo and why the destructor removes the reservation unconditionally.
//
// One caveat measured in Q13: ABM_REMOVE reports success but leaves GetLastError()
// at 87 (ERROR_INVALID_PARAMETER). That is the shell's own noise, not a failure -
// the work area came back exactly - so the code does not treat the error code as
// meaningful.

#pragma once

#include "platform/win_common.h"

#include <cstdint>

namespace dockforge::finderbar {
// A nested namespace so call sites read `appbar::ReserveTopStrip` and the reader can
// tell at a glance that this is the shell AppBar protocol, not the bar itself.
namespace appbar {

// The message the shell sends to the appbar's window. Declared here so the window
// procedure and ReserveTopStrip cannot disagree about it: ABM_NEW is told this value,
// and a mismatch would mean the shell talks to a window that never listens.
constexpr UINT kAppBarCallbackMessage = WM_APP + 62;

// Reserves a strip at the top of the primary monitor's work area for the bar.
//
// Returns true when the shell accepted the reservation. On failure the work area is
// untouched and the caller must fall back to auto-hide: a bar that covers windows is
// worse than a bar that hides.
//
// `height_px` is physical pixels, not DIP: the shell works in pixels and converting
// here would put the conversion in two places.
bool ReserveTopStrip(HWND window, int height_px);

// Releases the reservation. Safe to call when no reservation is held, and safe to
// call twice: the second call is a no-op rather than a second ABM_REMOVE, because the
// shell is not required to tolerate removing an appbar it does not know.
void ReleaseTopStrip();

// True while a reservation is held. The bar uses this to decide whether it may pin
// itself: without a reservation, pinning means covering a maximized window's title
// bar.
bool TopStripReserved();

// The reserved height in physical pixels, or 0 when nothing is reserved.
int ReservedStripPx();

// The work area the reservation produced. Valid only while TopStripReserved().
// Exposed so a probe can assert the shell really moved it rather than trusting the
// return code.
RECT ReservedWorkArea();

// Counters for the probe snapshot: how many times the shell asked us to reposition
// (ABN_POSCHANGED) and whether a fullscreen app currently owns the screen
// (ABN_FULLSCREENAPP). The second one matters because a fullscreen video must not
// have its top 28dip covered even when the strip is reserved.
struct AppBarStats {
    int registrations = 0;
    int position_changes = 0;
    bool fullscreen_app = false;
};

AppBarStats GetAppBarStats();

// The message the shell sends to the appbar's window. The bar's window procedure
// forwards it here; keeping the handler in this module means the bar does not have to
// know the ABN_* vocabulary.
void HandleAppBarMessage(WPARAM wparam, LPARAM lparam);

}  // namespace appbar
}  // namespace dockforge::finderbar
