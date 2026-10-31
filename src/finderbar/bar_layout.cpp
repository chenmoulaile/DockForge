#include "finderbar/bar_layout.h"

#include <algorithm>
#include <cmath>

namespace dockforge::finderbar {
namespace {

// Average advance width of a mixed-case Latin glyph at 1 em, for Segoe UI. Measured
// against the real D2D text layout rather than guessed: "File" at 13 DIP is 21.9 DIP
// wide, which is 4 * 13 * 0.421. Only ApproximateTextWidth uses it - production
// measures for real - but a test needs a deterministic number, and a fake that is
// 20% off from reality would make the layout tests assert the wrong widths.
constexpr float kAverageAdvanceEm = 0.421f;

// A single menu title never gets wider than this fraction of the bar. Without the cap
// a pathological title pushes every other title into the overflow and the bar shows
// one unusable item; with it, the title is truncated by the painter and the rest of
// the bar still works.
constexpr float kMaxTitleWidthFraction = 0.30f;

float ClampFloat(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

float TitleBoxWidth(const wchar_t* title, MeasureFn measure, float max_title) {
    const float text_width = measure(title, kMenuFontDip);
    return std::min(text_width, max_title) + kMenuTitlePaddingDip * 2.0f;
}

}  // namespace

float ApproximateTextWidth(const wchar_t* text, float size_dip) {
    if (text == nullptr || size_dip <= 0.0f) return 0.0f;
    std::size_t count = 0;
    for (const wchar_t* p = text; *p != L'\0'; ++p) ++count;
    return static_cast<float>(count) * size_dip * kAverageAdvanceEm;
}

std::size_t LayoutBar(const BarLayoutParams& params, const std::vector<std::wstring>& menu_titles,
                      MeasureFn measure, BarBox* out, std::size_t capacity,
                      BarLayoutSummary* summary) {
    if (out == nullptr || capacity == 0) {
        if (summary != nullptr) *summary = BarLayoutSummary{};
        return 0;
    }
    const MeasureFn measure_fn = measure != nullptr ? measure : &ApproximateTextWidth;
    const float bar_height = params.height_dip > 0.0f ? params.height_dip : kDefaultBarHeightDip;

    BarLayoutSummary local;

    // The trailing status area is reserved before anything is placed. A bar narrower
    // than the leading inset plus the status area has room for nothing, and the caller
    // needs that reported rather than silently getting negative-width boxes.
    const float status_width = std::max(0.0f, params.status_width_dip);
    const float usable_right = params.width_dip - status_width - kTrailingInsetDip;
    local.status_start_dip = std::max(0.0f, params.width_dip - status_width);

    float cursor = kLeadingInsetDip;
    std::size_t count = 0;

    const bool menus_present = params.show_app_menu && !menu_titles.empty();
    const float max_title = std::max(0.0f, params.width_dip * kMaxTitleWidthFraction);
    // Measured before the name so the name can leave room for it. The overflow box is
    // the escape hatch: if the name consumed the space it needed, a long document title
    // would leave a bar that shows a filename and no way to reach the menus at all.
    const float overflow_width =
        measure_fn(L"\x2026", kMenuFontDip) + kOverflowPaddingDip * 2.0f;

    // ---- system menu ----------------------------------------------------
    if (params.show_system_menu && count < capacity) {
        // The box is as tall as the bar so the whole height is clickable; the glyph
        // inside is 14 DIP and vertically centred by the painter.
        const float box_width = kSystemMenuGlyphDip + kMenuTitlePaddingDip;
        BarBox box;
        box.rect = render::RectF{cursor, 0.0f, cursor + box_width, bar_height};
        box.is_system_menu = true;
        out[count++] = box;
        cursor += box_width + kSystemMenuGapDip;
    }

    // ---- application name ------------------------------------------------
    if (params.show_app_name && !params.app_name.empty() && count < capacity) {
        const float text_width = measure_fn(params.app_name.c_str(), kAppNameFontDip);
        // The name is allowed to take whatever the menus leave, but it must not eat
        // the whole bar: if it did, the menus would all fold into the overflow and the
        // bar would show a filename instead of a menu bar. Two ceilings apply - 45% of
        // what is left, and whatever still leaves the overflow box a place to land.
        const float remaining = std::max(0.0f, usable_right - cursor);
        const float reserve = menus_present ? overflow_width + kMenuTitleGapDip : 0.0f;
        const float max_width = std::max(0.0f, std::min(remaining * 0.45f, remaining - reserve));
        const float box_width = std::min(text_width, max_width);
        BarBox box;
        box.rect = render::RectF{cursor, 0.0f, cursor + box_width, bar_height};
        box.label = params.app_name;
        box.is_app_name = true;
        out[count++] = box;
        cursor += box_width + kNameMenuGapDip;
    }

    // ---- application menus ----------------------------------------------
    if (menus_present) {
        const std::size_t limit = static_cast<std::size_t>(std::max(1, params.max_menu_items));

        // Two passes. The first measures, the second places. A single pass would have
        // to guess whether the overflow box is needed, and guessing wrong either
        // leaves a title clipped by the status area or reserves width for an overflow
        // box that never appears.
        std::vector<float> widths;
        widths.reserve(menu_titles.size());
        for (const std::wstring& title : menu_titles) {
            widths.push_back(TitleBoxWidth(title.c_str(), measure_fn, max_title));
        }

        // How many titles fit if the overflow box is NOT needed.
        std::size_t plain_fit = 0;
        float plain_run = 0.0f;
        for (std::size_t i = 0; i < widths.size() && plain_fit < limit; ++i) {
            const float extra = widths[i] + (plain_fit == 0 ? 0.0f : kMenuTitleGapDip);
            if (cursor + plain_run + extra > usable_right) break;
            plain_run += extra;
            ++plain_fit;
        }

        // Everything fits within the limit: place them all, no overflow box.
        if (plain_fit == widths.size()) {
            for (std::size_t i = 0; i < plain_fit && count < capacity; ++i) {
                if (i > 0) cursor += kMenuTitleGapDip;
                BarBox box;
                box.rect = render::RectF{cursor, 0.0f, cursor + widths[i], bar_height};
                box.label = menu_titles[i];
                box.menu_index = i;
                out[count++] = box;
                cursor += widths[i];
            }
            local.placed_menus = plain_fit;
        } else {
            // Something has to fold. Reserve the overflow box first, then place as
            // many titles as fit beside it - the reverse order would place a title
            // and then discover the box does not fit and have to unplace it.
            //
            // The gap before the overflow box is always counted, even when it turns out
            // no title is placed and the gap is then not drawn. Being 18 DIP pessimistic
            // costs at most one title on a bar that is already collapsing; being
            // optimistic puts the overflow box past the status area, which is a visible
            // bug rather than a slightly conservative layout.
            std::size_t fit = 0;
            float run = 0.0f;
            for (std::size_t i = 0; i < widths.size() && fit < limit; ++i) {
                const float extra = widths[i] + (fit == 0 ? 0.0f : kMenuTitleGapDip);
                if (cursor + run + extra + kMenuTitleGapDip + overflow_width > usable_right) {
                    break;
                }
                run += extra;
                ++fit;
            }

            for (std::size_t i = 0; i < fit && count < capacity; ++i) {
                if (i > 0) cursor += kMenuTitleGapDip;
                BarBox box;
                box.rect = render::RectF{cursor, 0.0f, cursor + widths[i], bar_height};
                box.label = menu_titles[i];
                box.menu_index = i;
                out[count++] = box;
                cursor += widths[i];
            }
            local.placed_menus = fit;

            // The overflow box is the only way to reach a folded menu, so it is worth
            // drawing even when no single title fits beside it: a bar that shows nothing
            // but a filename cannot open any menu at all. It is placed without the
            // leading gap in that case, because there is no title for the gap to
            // separate it from.
            const float overflow_left = cursor + (fit > 0 ? kMenuTitleGapDip : 0.0f);
            if (count < capacity && overflow_left + overflow_width <= usable_right) {
                cursor = overflow_left;
                BarBox box;
                box.rect = render::RectF{cursor, 0.0f, cursor + overflow_width, bar_height};
                box.label = L"\x2026";
                box.is_overflow = true;
                out[count++] = box;
                cursor += overflow_width;
                local.overflowed = true;
            }
        }
    }

    local.left_group_width_dip = cursor;
    // Kept from the earlier decision: a bar too narrow for even the overflow box has
    // already been reported as clipped, and `cursor > usable_right` cannot see that
    // because nothing was drawn past the edge.
    if (cursor > usable_right) local.clipped = true;
    if (summary != nullptr) *summary = local;
    return count;
}

std::vector<BarBox> LayoutBar(const BarLayoutParams& params,
                              const std::vector<std::wstring>& menu_titles, MeasureFn measure,
                              BarLayoutSummary* summary) {
    // Bounded by the inputs: the glyph box, the name box, one box per title and the
    // overflow box.
    const std::size_t capacity = menu_titles.size() + 3;
    std::vector<BarBox> boxes(capacity);
    const std::size_t written =
        LayoutBar(params, menu_titles, measure, boxes.data(), boxes.size(), summary);
    boxes.resize(written);
    return boxes;
}

std::size_t BoxIndexAt(const BarBox* boxes, std::size_t count, float x_dip, float y_dip) {
    if (boxes == nullptr) return kNoMenu;
    for (std::size_t i = 0; i < count; ++i) {
        const render::RectF& r = boxes[i].rect;
        if (x_dip < r.left || x_dip >= r.right) continue;
        if (y_dip < r.top || y_dip >= r.bottom) continue;
        return i;
    }
    return kNoMenu;
}

std::size_t HitTestBar(const BarBox* boxes, std::size_t count, float x_dip, float y_dip) {
    const std::size_t index = BoxIndexAt(boxes, count, x_dip, y_dip);
    if (index == kNoMenu) return kNoMenu;
    return boxes[index].menu_index;
}

void AdvanceAutoHide(AutoHideState& state, bool pointer_in_trigger, bool pointer_in_bar,
                     double delta_ms) {
    // A stalled frame must not teleport the bar. 64ms is the same clamp the dock uses
    // for its own animations (kMaxAnimationStepMs), so a hiccup behaves identically on
    // every surface instead of each one inventing its own tolerance.
    const double step = std::min(std::max(delta_ms, 0.0), 64.0);
    const float rate = static_cast<float>(step / kAutoHideDurationMs);

    // Staying open while the pointer is over the bar is what makes a menu usable:
    // retracting under an open menu would move the click target mid-interaction.
    const bool open = pointer_in_trigger || pointer_in_bar;
    state.shown = open;
    state.progress = open ? ClampFloat(state.progress + rate, 0.0f, 1.0f)
                          : ClampFloat(state.progress - rate, 0.0f, 1.0f);
}

float AutoHideOffsetDip(const AutoHideState& state, float height_dip) {
    const float height = height_dip > 0.0f ? height_dip : kDefaultBarHeightDip;
    const float travel = std::max(0.0f, height - kAutoHideHairlineDip);
    // Smoothstep: ease-out on the way in, ease-in on the way out. The asymmetry is
    // what makes the reveal feel responsive and the retraction feel unhurried, which
    // is the same shape macOS uses for its menu bar.
    const float t = ClampFloat(state.progress, 0.0f, 1.0f);
    const float eased = t * t * (3.0f - 2.0f * t);
    return -travel * (1.0f - eased);
}

bool AutoHideInteractive(const AutoHideState& state) {
    // Past half-revealed the user is clearly aiming at the bar. Below that it is a
    // hairline, and eating a click meant for the desktop is worse than a missed one:
    // the user can always come back, but a swallowed click looks like a broken system.
    return state.progress >= 0.5f;
}

}  // namespace dockforge::finderbar
