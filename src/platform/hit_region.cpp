// DockForge - clickable region for a dock window (T-010).
#include "platform/hit_region.h"

#include "core/log.h"

#include <algorithm>

namespace dockforge::platform {

HRGN CreateDockRegion(const RECT& plate, int radius_px, const RECT& row, bool include_row) {
    const int plate_width = plate.right - plate.left;
    const int plate_height = plate.bottom - plate.top;
    if (plate_width <= 0 || plate_height <= 0) return nullptr;

    // The rounded plate is the base: a rounded rect for the corners, with the
    // radius clamped to half the short side so a tall radius cannot invert it.
    const int radius = std::max(0, std::min(radius_px, std::min(plate_width, plate_height) / 2));
    HRGN region = CreateRoundRectRgn(plate.left, plate.top, plate.right + 1, plate.bottom + 1,
                                     radius * 2, radius * 2);
    if (region == nullptr) {
        DF_WARN("platform", "CreateRoundRectRgn failed: %lu", GetLastError());
        return nullptr;
    }

    if (!include_row) return region;

    const int row_width = row.right - row.left;
    const int row_height = row.bottom - row.top;
    if (row_width <= 0 || row_height <= 0) return region;

    // The magnified icons overhang above the plate. Their area is a rectangle: the
    // icons themselves are square-ish and the row is contiguous, so a rounded
    // region would carve away pixels the user can see and expects to be able to
    // click.
    HRGN row_region = CreateRectRgn(row.left, row.top, row.right, row.bottom);
    if (row_region == nullptr) {
        DF_WARN("platform", "CreateRectRgn failed: %lu", GetLastError());
        return region;
    }
    // CombineRgn takes ownership of nothing; both inputs survive and the caller
    // must free them. RGN_OR keeps both areas clickable.
    if (CombineRgn(region, region, row_region, RGN_OR) == RGN_ERROR) {
        DF_WARN("platform", "CombineRgn failed: %lu", GetLastError());
    }
    DeleteObject(row_region);
    return region;
}

bool ApplyWindowRegion(HWND hwnd, HRGN region) {
    if (hwnd == nullptr) {
        if (region != nullptr) DeleteObject(region);
        return false;
    }
    // SetWindowRgn takes ownership on success. On failure the caller keeps it, so
    // free it here to avoid a leak on the error path.
    if (SetWindowRgn(hwnd, region, TRUE) == 0) {
        DF_WARN("platform", "SetWindowRgn failed: %lu", GetLastError());
        if (region != nullptr) DeleteObject(region);
        return false;
    }
    return true;
}

}  // namespace dockforge::platform
