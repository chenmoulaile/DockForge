// DockForge - the right-region status glyphs implementation (see status_icons.h).
#include "finderbar/status_icons.h"

#include "finderbar/bar_widgets.h"  // kWidgetCriticalColor - the frozen HIG systemRed

#include <algorithm>
#include <cmath>

namespace dockforge::finderbar {
namespace {

// Disc + bar primitives, verbatim the dock_view.cpp pattern (a circle is a rounded
// rect with radius = half width; the C-04 backend contract has no ellipse call).
void FillCircle(render::IRenderBackend& backend, float cx, float cy, float radius,
                const render::ColorF& color) {
    const render::RectF rect{cx - radius, cy - radius, cx + radius, cy + radius};
    backend.FillRoundedRect(rect, radius, color);
}

constexpr float kBatteryFillInset = 2.3f;
constexpr float kBatteryStrokeWidth = 1.3f;

}  // namespace

// ---- battery ------------------------------------------------------------------

void DrawBatteryGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                      int percent, bool charging, bool alert,
                      const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    // Proportions of the macOS menu-bar battery, from the round10 top-bar
    // measurement (outer frame ~25x12 DIP at 100%): body 23x12 + cap 2x5. The
    // slot is the layout's icon rect, so the body height follows the slot's
    // vertical space and the 25:12 frame falls out of the width.
    const float body_h = std::min(slot.Height() * 0.75f, 12.0f);
    const float total_w = slot.Width();
    const float cap_w = std::max(1.5f, total_w * 0.08f);
    const float cap_h = body_h * 0.42f;
    const float body_w = std::max(1.0f, total_w - cap_w);
    const float cy = slot.CenterY();
    const float body_left = slot.left;
    const render::RectF body{body_left, cy - body_h * 0.5f, body_left + body_w,
                             cy + body_h * 0.5f};
    const float radius = body_h * 0.28f;

    backend.StrokeRoundedRect(body, radius, color, kBatteryStrokeWidth);
    const render::RectF cap{body.right + cap_w * 0.25f, cy - cap_h * 0.5f,
                            body.right + cap_w * 1.25f, cy + cap_h * 0.5f};
    backend.FillRoundedRect(cap, cap_w * 0.5f, color);

    // Level bar, inset from the stroke. Alert (<20%) paints it in the frozen HIG
    // systemRed; the normal state uses the glyph colour like every other item.
    const float level =
        std::clamp(static_cast<float>(percent), 0.0f, 100.0f) / 100.0f;
    const float inner_w = body_w - kBatteryFillInset * 2.0f;
    const float fill_w = inner_w * level;
    if (fill_w > 0.5f) {
        const render::RectF fill{body.left + kBatteryFillInset, body.top + kBatteryFillInset,
                                 body.left + kBatteryFillInset + fill_w,
                                 body.bottom - kBatteryFillInset};
        backend.FillRoundedRect(fill, radius * 0.6f,
                                alert ? kWidgetCriticalColor : color);
    }

    // Charging bolt: a three-step zig-zag inside the body (the same stepped-stroke
    // trick the weather glyph uses for diagonals - the round5 zoom evidence shows
    // the bolt inside the outline body). Kept clear of the level bar's fill so the
    // two never merge into a blob.
    if (charging) {
        const float cx = body.CenterX();
        const float step = body_h * 0.16f;
        const float y0 = body.top + body_h * 0.26f;
        // top-right notch, middle-left notch, bottom-right notch
        FillCircle(backend, cx + step, y0, step * 0.85f, color);
        FillCircle(backend, cx - step * 0.6f, y0 + step * 1.5f, step * 0.85f, color);
        FillCircle(backend, cx + step * 0.8f, y0 + step * 3.0f, step * 0.85f, color);
    }
}

// ---- wifi / ethernet ----------------------------------------------------------

WifiGlyphGeometry WifiGlyphGeometry::Compute(const render::RectF& slot) {
    WifiGlyphGeometry g;
    // The apex sits low-centred; the three arcs open upward (y-down screen space:
    // the span runs from -135 deg to -45 deg, i.e. up-left to up-right).
    g.apex_x = slot.CenterX();
    g.apex_y = slot.bottom - slot.Height() * 0.12f;
    g.arc_count = 3;
    g.dots_per_arc = 7;
    g.dot_radius = std::max(0.9f, slot.Width() * 0.065f);
    const float base = slot.Width() * 0.17f;
    const float step = slot.Width() * 0.175f;
    g.radii[0] = base;
    g.radii[1] = base + step;
    g.radii[2] = base + step * 2.0f;
    g.start_deg = -135.0f;
    g.end_deg = -45.0f;
    return g;
}

void DrawWifiGlyph(render::IRenderBackend& backend, const render::RectF& slot, int bars,
                   const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const WifiGlyphGeometry g = WifiGlyphGeometry::Compute(slot);

    // Apex dot: lit whenever the item exists (an Ethernet machine shows its own
    // glyph; the apex belongs to the Wi-Fi shape only).
    FillCircle(backend, g.apex_x, g.apex_y, g.dot_radius * 1.15f, color);

    const float rad_per_deg = 3.14159265358979f / 180.0f;
    for (int arc = 0; arc < g.arc_count; ++arc) {
        const bool lit = arc < std::clamp(bars, 0, 4);
        render::ColorF arc_color = color;
        if (!lit) arc_color.a *= 0.28f;
        const float radius = g.radii[arc];
        for (int i = 0; i < g.dots_per_arc; ++i) {
            const float t = static_cast<float>(i) /
                            static_cast<float>(g.dots_per_arc - 1);
            const float deg = g.start_deg + (g.end_deg - g.start_deg) * t;
            const float x = g.apex_x + radius * std::cos(deg * rad_per_deg);
            const float y = g.apex_y + radius * std::sin(deg * rad_per_deg);
            FillCircle(backend, x, y, g.dot_radius, arc_color);
        }
    }
}

void DrawEthernetGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                       const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    // The plug: a low rounded-rect body with three pins on top, centred in the
    // slot. Proportions follow the slot like every other glyph here.
    const float body_w = slot.Width() * 0.72f;
    const float body_h = slot.Height() * 0.42f;
    const float cx = slot.CenterX();
    const float body_bottom = slot.bottom - slot.Height() * 0.12f;
    const float body_top = body_bottom - body_h;
    const float body_left = cx - body_w * 0.5f;
    const float body_right = cx + body_w * 0.5f;
    const float radius = body_h * 0.25f;
    backend.StrokeRoundedRect(render::RectF{body_left, body_top, body_right, body_bottom},
                              radius, color, 1.3f);
    const float pin_h = slot.Height() * 0.22f;
    const float pin_w = std::max(1.2f, body_w * 0.09f);
    for (int i = -1; i <= 1; ++i) {
        const float px = cx + static_cast<float>(i) * body_w * 0.26f;
        backend.FillRoundedRect(render::RectF{px - pin_w * 0.5f, body_top - pin_h,
                                              px + pin_w * 0.5f, body_top},
                                pin_w * 0.5f, color);
    }
}

}  // namespace dockforge::finderbar
