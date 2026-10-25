#include "dock/launchpad_search.h"

#include <algorithm>
#include <cmath>

namespace dockforge::dock {
namespace {

// The glyph advance used to place the caret. A fixed advance is deliberate: a
// proportional font makes the caret position depend on the exact font file the
// system hands over, and a search field whose caret drifts after a font update
// is worse than one that is slightly approximate. The visual result at the sizes
// used here is indistinguishable, and the caret only ever needs to land near the
// right edge of a typed prefix.
constexpr float kSearchAdvanceFactor = 0.52f;

}  // namespace

render::RectF LaunchpadSearchRect(float width_dip, float height_dip) {
    const float clamped_width = std::min(kLaunchpadSearchWidthDip, width_dip - 32.0f);
    const float field_width = std::max(160.0f, clamped_width);
    const float left = std::max(16.0f, (width_dip - field_width) * 0.5f);
    const float top = (kLaunchpadTopBandDip - kLaunchpadSearchHeightDip) * 0.5f;    return render::RectF{left, top, left + field_width, top + kLaunchpadSearchHeightDip};
}

bool LaunchpadSearchHit(float x_dip, float y_dip, float width_dip, float height_dip) {
    const render::RectF field = LaunchpadSearchRect(width_dip, height_dip);
    return x_dip >= field.left && x_dip <= field.right && y_dip >= field.top &&
           y_dip <= field.bottom;
}

render::RectF LaunchpadSearchCaretRect(const std::wstring& text, int caret_index,
                                       const render::RectF& field) {
    if (caret_index < 0 || static_cast<std::size_t>(caret_index) > text.size()) {
        return render::RectF{field.left, field.top, field.left, field.bottom};
    }
    const float advance = kLaunchpadSearchFontDip * kSearchAdvanceFactor;
    const float x = field.left + kLaunchpadSearchInsetDip + advance * caret_index;
    const float clamped = std::min(x, field.right - 2.0f);
    return render::RectF{clamped, field.top + 8.0f, clamped + 1.5f, field.bottom - 8.0f};
}

render::RectF LaunchpadFolderRenameCaretRect(const std::wstring& text, int caret_index,
                                             const render::RectF& chip) {
    if (caret_index < 0 || static_cast<std::size_t>(caret_index) > text.size()) {
        return render::RectF{chip.left, chip.top, chip.left, chip.bottom};
    }
    const float advance = kLaunchpadFolderChipFontDip * kSearchAdvanceFactor;
    const float x = chip.left + kLaunchpadFolderChipPadDip * 0.5f + advance * caret_index;
    const float clamped = std::min(x, chip.right - 2.0f);
    return render::RectF{clamped, chip.top + 6.0f, clamped + 1.5f, chip.bottom - 6.0f};
}

void LaunchpadFolderChipRects(float width_dip, int count, std::vector<render::RectF>& rects) {
    rects.clear();
    if (count <= 0) return;
    if (width_dip <= 0.0f) return;

    rects.reserve(static_cast<std::size_t>(count));
    const float top = kLaunchpadFolderStripTopDip;
    const float bottom = top + kLaunchpadFolderChipHeightDip;
    // Every chip is the same width: a proportional chip strip makes the hit test
    // depend on the label text, and the labels change as folders are renamed
    // while the Launchpad is open.
    const float chip_width = kLaunchpadFolderChipWidthDip;
    float left = 16.0f;
    for (int i = 0; i < count; ++i) {
        if (left + chip_width > width_dip - 16.0f) break;  // clipped
        rects.push_back(render::RectF{left, top, left + chip_width, bottom});
        left += chip_width + kLaunchpadFolderChipGapDip;
    }
}

int LaunchpadFolderChipHit(float x_dip, float y_dip, const std::vector<render::RectF>& rects) {
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const render::RectF& r = rects[i];
        if (x_dip >= r.left && x_dip <= r.right && y_dip >= r.top && y_dip <= r.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int LaunchpadFolderChipLimit(float width_dip) {
    if (width_dip <= 0.0f) return 0;
    const float usable = width_dip - 32.0f;
    if (usable < kLaunchpadFolderChipWidthDip) return 0;
    return static_cast<int>(std::floor((usable + kLaunchpadFolderChipGapDip) /
                                       (kLaunchpadFolderChipWidthDip +
                                        kLaunchpadFolderChipGapDip)));
}

}  // namespace dockforge::dock
