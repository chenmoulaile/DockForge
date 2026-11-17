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

// ADR-0058 proportions, from the acceptance's round10 pixel scan: the battery is a
// VERTICAL outlined body with a cap on TOP (the reference reads ~10 px wide and
// ~30 px tall on the 2x host, i.e. a 5x15 DIP glyph). The old horizontal 25x12 DIP
// pill is gone.
constexpr float kBatteryFillInset = 1.5f;
constexpr float kBatteryStrokeWidth = 1.3f;

}  // namespace

// ---- battery ------------------------------------------------------------------

void DrawBatteryGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                      int percent, bool charging, bool alert,
                      const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float cx = slot.CenterX();
    // The cap sits on TOP of the body; the body fills the rest of the slot below it.
    const float cap_w = std::max(2.0f, slot.Width() * 0.42f);
    const float cap_h = std::max(1.2f, slot.Height() * 0.07f);
    const float body_top = slot.top + cap_h * 1.8f;
    const float body_w = std::max(3.0f, slot.Width() * 0.66f);
    const float body_h = slot.bottom - body_top;
    const render::RectF body{cx - body_w * 0.5f, body_top, cx + body_w * 0.5f, slot.bottom};
    const float radius = body_w * 0.32f;

    // Outline body (white), cap (dimmer - the reference's "grey hat").
    backend.StrokeRoundedRect(body, radius, color, kBatteryStrokeWidth);
    render::ColorF cap_color = color;
    cap_color.a *= 0.55f;
    backend.FillRoundedRect(render::RectF{cx - cap_w * 0.5f, slot.top, cx + cap_w * 0.5f,
                                          slot.top + cap_h},
                            cap_w * 0.5f, cap_color);

    // Level bar, inset from the stroke, filling from the BOTTOM upward. Alert (<20%)
    // paints it in the frozen HIG systemRed; the normal state uses the glyph colour.
    const float level =
        std::clamp(static_cast<float>(percent), 0.0f, 100.0f) / 100.0f;
    const float inner_h = body_h - kBatteryFillInset * 2.0f;
    const float fill_h = inner_h * level;
    if (fill_h > 0.5f) {
        const render::RectF fill{body.left + kBatteryFillInset,
                                 body.bottom - kBatteryFillInset - fill_h,
                                 body.right - kBatteryFillInset,
                                 body.bottom - kBatteryFillInset};
        backend.FillRoundedRect(fill, std::min(radius * 0.6f, fill_h * 0.5f),
                                alert ? kWidgetCriticalColor : color);
    }

    // Charging bolt: a stepped zig-zag climbing the body (the same stepped-square
    // trick the weather glyph uses for diagonals - the C-04 set has no rotation).
    if (charging) {
        const float step = body_w * 0.30f;
        const float y0 = body.top + body_h * 0.24f;
        FillCircle(backend, cx, y0, step * 0.85f, color);
        FillCircle(backend, cx - step * 0.5f, y0 + step * 1.6f, step * 0.85f, color);
        FillCircle(backend, cx + step * 0.4f, y0 + step * 3.2f, step * 0.85f, color);
    }
}

// ---- wifi / ethernet ----------------------------------------------------------

WifiGlyphGeometry WifiGlyphGeometry::Compute(const render::RectF& slot) {
    WifiGlyphGeometry g;
    // The apex sits low-centred; the three arcs open upward (y-down screen space:
    // the span runs from -135 deg to -45 deg, i.e. up-left to up-right). The band
    // radius is the SOLID-stroke width from the acceptance measurement: the
    // reference arcs read as continuous strokes, so the discs overlap densely.
    g.apex_x = slot.CenterX();
    g.apex_y = slot.bottom - slot.Height() * 0.12f;
    g.arc_count = 3;
    g.dot_radius = std::max(0.9f, slot.Width() * 0.085f);
    const float base = slot.Width() * 0.17f;
    const float step = slot.Width() * 0.175f;
    g.radii[0] = base;
    g.radii[1] = base + step;
    g.radii[2] = base + step * 2.0f;
    g.start_deg = -135.0f;
    g.end_deg = -45.0f;
    // Steps sized so neighbouring discs overlap by ~55%: the chain reads as one
    // continuous band, not a dotted line.
    const float rad_per_deg = 3.14159265358979f / 180.0f;
    const float outer_len =
        g.radii[2] * (g.end_deg - g.start_deg) * rad_per_deg;
    const float stride = std::max(0.6f, g.dot_radius * 0.85f);
    g.steps_per_arc = std::max(6, static_cast<int>(std::lround(outer_len / stride)));
    return g;
}

void DrawWifiGlyph(render::IRenderBackend& backend, const render::RectF& slot, int bars,
                   const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const WifiGlyphGeometry g = WifiGlyphGeometry::Compute(slot);

    // Apex dot: lit whenever the item exists (an Ethernet machine shows its own
    // glyph; the apex belongs to the Wi-Fi shape only).
    FillCircle(backend, g.apex_x, g.apex_y, g.dot_radius * 1.1f, color);

    const float rad_per_deg = 3.14159265358979f / 180.0f;
    for (int arc = 0; arc < g.arc_count; ++arc) {
        const bool lit = arc < std::clamp(bars, 0, 4);
        render::ColorF arc_color = color;
        if (!lit) arc_color.a *= 0.28f;
        const float radius = g.radii[arc];
        for (int i = 0; i < g.steps_per_arc; ++i) {
            const float t = static_cast<float>(i) /
                            static_cast<float>(g.steps_per_arc - 1);
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

// ---- notification bell / tray chevron (ADR-0058) ------------------------------

void DrawBellGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float w = slot.Width();
    const float h = slot.Height();
    const float cx = slot.CenterX();
    // Dome: a disc crowned over a slightly flared skirt, the macOS bell silhouette
    // built from the C-04 primitive set only.
    const float dome_r = w * 0.26f;
    const float dome_cy = slot.top + h * 0.30f;
    const float skirt_top = dome_cy + dome_r * 0.15f;
    const float skirt_h = h * 0.30f;
    const float skirt_w = w * 0.62f;
    backend.FillRoundedRect(render::RectF{cx - skirt_w * 0.5f, skirt_top,
                                          cx + skirt_w * 0.5f, skirt_top + skirt_h},
                            skirt_w * 0.22f, color);
    FillCircle(backend, cx, dome_cy, dome_r, color);
    // Bottom lip: a wider bar where the dome meets the skirt.
    const float lip_w = w * 0.80f;
    const float lip_h = h * 0.09f;
    backend.FillRoundedRect(render::RectF{cx - lip_w * 0.5f, skirt_top + skirt_h - lip_h * 0.5f,
                                          cx + lip_w * 0.5f, skirt_top + skirt_h + lip_h * 0.5f},
                            lip_h * 0.5f, color);
    // Clapper dot beneath.
    FillCircle(backend, cx, slot.bottom - h * 0.06f, w * 0.09f, color);
}

void DrawTrayGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float w = slot.Width();
    const float h = slot.Height();
    const float cx = slot.CenterX();
    // Upward caret: a stepped diagonal of overlapping squares - left half climbs,
    // right half mirrors it down. Square size reads as a stroke of ~2.5 DIP at the
    // 16 DIP icon box, matching the other glyph stroke weights.
    const float s = std::max(1.4f, w * 0.16f);
    const float apex_y = slot.top + h * 0.26f;
    const float base_y = slot.bottom - h * 0.30f;
    const int steps = 3;
    for (int i = 0; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float x0 = cx - w * 0.34f + w * 0.30f * t;
        const float y0 = base_y - (base_y - apex_y) * t;
        backend.FillRoundedRect(render::RectF{x0 - s * 0.5f, y0 - s * 0.5f,
                                              x0 + s * 0.5f, y0 + s * 0.5f},
                                s * 0.35f, color);
        const float x1 = cx + w * 0.34f - w * 0.30f * t;
        backend.FillRoundedRect(render::RectF{x1 - s * 0.5f, y0 - s * 0.5f,
                                              x1 + s * 0.5f, y0 + s * 0.5f},
                                s * 0.35f, color);
    }
    // Apex: the two diagonals meet at the top centre.
    backend.FillRoundedRect(render::RectF{cx - s * 0.5f, apex_y - s * 0.5f,
                                          cx + s * 0.5f, apex_y + s * 0.5f},
                            s * 0.35f, color);
}

void DrawVolumeGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                     const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float w = slot.Width();
    const float h = slot.Height();
    const float cx = slot.CenterX();
    // Speaker: a boxy body on the left half, a cone flaring right (stepped, no
    // rotation in the C-04 set), then one sound-wave arc band. Solid, like the
    // reference's other white fixtures.
    const float body_w = w * 0.16f;
    const float body_h = h * 0.34f;
    const float body_left = cx - w * 0.34f;
    const float body_cy = slot.CenterY() - h * 0.02f;
    backend.FillRoundedRect(
        render::RectF{body_left, body_cy - body_h * 0.5f, body_left + body_w,
                      body_cy + body_h * 0.5f},
        body_w * 0.2f, color);
    // Cone: three steps widening from the body to the right edge.
    const float cone_base = body_left + body_w;
    for (int i = 0; i < 3; ++i) {
        const float t = static_cast<float>(i) / 3.0f;
        const float seg_w = w * 0.13f;
        const float seg_h = body_h * (0.55f + 0.75f * t);
        const float x = cone_base + seg_w * t;
        backend.FillRoundedRect(
            render::RectF{x, body_cy - seg_h * 0.5f, x + seg_w + 0.6f,
                          body_cy + seg_h * 0.5f},
            1.0f, color);
    }
    // One sound-wave arc band (a short vertical-ish chain of discs, reading as the
    // first wave).
    const float wave_r = w * 0.30f;
    const float wave_cx = cone_base + w * 0.10f;
    for (int i = 0; i < 4; ++i) {
        const float t = static_cast<float>(i) / 3.0f;
        const float deg = -60.0f + 60.0f * t;
        const float rad = deg * 3.14159265358979f / 180.0f;
        const float dot = std::max(0.8f, w * 0.055f);
        FillCircle(backend, wave_cx + wave_r * std::sin(rad), body_cy - wave_r * std::cos(rad),
                   dot, color);
    }
}

void DrawSpotlightGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                        const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float w = slot.Width();
    const float h = slot.Height();
    // Lens: an outlined circle up-left (stroke ring from a filled disc with a
    // punched centre is not possible in the C-04 set, so the ring is a chain of
    // discs along a circle path - the wifi glyph's solid-band trick, one band).
    const float lens_r = std::min(w, h) * 0.28f;
    const float lens_cx = slot.left + w * 0.40f;
    const float lens_cy = slot.top + h * 0.40f;
    const int steps = 14;
    const float stroke = std::max(0.9f, w * 0.075f);
    for (int i = 0; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float rad = t * 6.2831853f;
        FillCircle(backend, lens_cx + lens_r * std::cos(rad), lens_cy + lens_r * std::sin(rad),
                   stroke, color);
    }
    // Handle: stepped squares running down-right from the lens edge.
    const float s = std::max(1.2f, w * 0.13f);
    for (int i = 0; i < 4; ++i) {
        const float t = static_cast<float>(i);
        const float x = lens_cx + lens_r * 0.9f + s * 0.9f * t;
        const float y = lens_cy + lens_r * 0.9f + s * 0.9f * t;
        backend.FillRoundedRect(render::RectF{x - s * 0.5f, y - s * 0.5f, x + s * 0.5f,
                                              y + s * 0.5f},
                                s * 0.35f, color);
    }
}

}  // namespace dockforge::finderbar
