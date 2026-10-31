#include "finderbar/appbar.h"

#include "core/log.h"

namespace dockforge::finderbar {
namespace appbar {
namespace {

constexpr const char* kModule = "finderbar";

// All the reservation state lives here rather than on the FinderBar object because
// the shell identifies an appbar by HWND, and a second FinderBar instance must not be
// able to release the first one's strip behind its back. One process, one strip.
HWND g_window = nullptr;
int g_reserved_px = 0;
RECT g_work{};
AppBarStats g_stats;

}  // namespace

bool ReserveTopStrip(HWND window, int height_px) {
    if (window == nullptr || height_px <= 0) return false;
    if (g_window != nullptr) {
        // Already registered. Re-issuing ABM_NEW would be rejected; a height change
        // is handled by the caller calling Release then Reserve.
        if (g_window == window && g_reserved_px == height_px) return true;
        ReleaseTopStrip();
    }

    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        DF_WARN(kModule, "SPI_GETWORKAREA failed err=%lu; not reserving the strip", GetLastError());
        return false;
    }

    APPBARDATA register_data{};
    register_data.cbSize = sizeof(register_data);
    register_data.hWnd = window;
    register_data.uCallbackMessage = kAppBarCallbackMessage;
    if (SHAppBarMessage(ABM_NEW, &register_data) == FALSE) {
        DF_WARN(kModule, "ABM_NEW refused; the bar cannot reserve space");
        return false;
    }

    // ABM_QUERYPOS then ABM_SETPOS. The query lets the shell shrink the rect (it does
    // when another appbar already owns part of the edge), and setting the queried rect
    // is the documented order.
    APPBARDATA query{};
    query.cbSize = sizeof(query);
    query.hWnd = window;
    query.uEdge = ABE_TOP;
    query.rc = work;
    query.rc.bottom = work.top + height_px;
    SHAppBarMessage(ABM_QUERYPOS, &query);

    APPBARDATA set = query;
    set.cbSize = sizeof(set);
    set.hWnd = window;
    set.uEdge = ABE_TOP;
    if (SHAppBarMessage(ABM_SETPOS, &set) == FALSE) {
        DF_WARN(kModule, "ABM_SETPOS refused; releasing the registration");
        APPBARDATA remove{};
        remove.cbSize = sizeof(remove);
        remove.hWnd = window;
        SHAppBarMessage(ABM_REMOVE, &remove);
        return false;
    }

    g_window = window;
    g_reserved_px = set.rc.bottom - set.rc.top;
    ++g_stats.registrations;

    // Read the work area back rather than computing it. Q11 measured that the shell's
    // answer is the only reliable source: the rect ABM_SETPOS hands back is what we
    // asked for, and the work area is what the shell actually did with it.
    RECT observed{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &observed, 0)) {
        g_work = observed;
    } else {
        g_work = work;
        g_work.top = work.top + g_reserved_px;
    }

    DF_INFO(kModule, "reserved top strip height_px=%d work=%ld,%ld,%ld,%ld", g_reserved_px, g_work.left,
            g_work.top, g_work.right, g_work.bottom);
    return true;
}

void ReleaseTopStrip() {
    if (g_window == nullptr) return;
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    // Q13 measured ABM_REMOVE returning success with GetLastError() left at 87
    // (ERROR_INVALID_PARAMETER). The work area is restored correctly, so the error
    // code is the shell's noise and is deliberately not treated as a failure.
    SHAppBarMessage(ABM_REMOVE, &data);
    DF_INFO(kModule, "released the top strip");
    g_window = nullptr;
    g_reserved_px = 0;
    g_work = RECT{};
}

bool TopStripReserved() { return g_window != nullptr; }

int ReservedStripPx() { return g_reserved_px; }

RECT ReservedWorkArea() { return g_work; }

AppBarStats GetAppBarStats() { return g_stats; }

void HandleAppBarMessage(WPARAM wparam, LPARAM lparam) {
    switch (wparam) {
        case ABN_POSCHANGED:
            ++g_stats.position_changes;
            // The shell tells us another appbar moved. A production bar would re-run
            // ABM_QUERYPOS here; the strip is full-width on the primary monitor, so
            // there is nothing to renegotiate and the counter is enough to prove the
            // callback arrives.
            DF_DEBUG(kModule, "ABN_POSCHANGED #%d", g_stats.position_changes);
            break;
        case ABN_FULLSCREENAPP:
            g_stats.fullscreen_app = lparam != 0;
            // A fullscreen app must not have its top strip covered. The bar reads this
            // through GetAppBarStats and hides itself while it is set.
            DF_DEBUG(kModule, "ABN_FULLSCREENAPP active=%d", g_stats.fullscreen_app ? 1 : 0);
            break;
        case ABN_STATECHANGE:
            DF_DEBUG(kModule, "ABN_STATECHANGE");
            break;
        default:
            DF_DEBUG(kModule, "appbar message %u", static_cast<unsigned>(wparam));
            break;
    }
}

}  // namespace appbar
}  // namespace dockforge::finderbar
