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

// A chain of overlapping discs along a circle arc - the C-04 set has no arc
// primitive, and dense overlap (stride ~55% of the radius) is what makes the
// chain read as one solid stroke.
void FillArcBand(render::IRenderBackend& backend, float cx, float cy, float radius,
                 float start_deg, float end_deg, float dot_radius,
                 const render::ColorF& color) {
    const float rad_per_deg = 3.14159265358979f / 180.0f;
    const float arc_len = radius * (end_deg - start_deg) * rad_per_deg;
    const float stride = std::max(0.5f, dot_radius * 0.85f);
    const int steps =
        std::max(6, static_cast<int>(std::lround(arc_len / stride)));
    for (int i = 0; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps - 1);
        const float deg = start_deg + (end_deg - start_deg) * t;
        const float rad = deg * rad_per_deg;
        FillCircle(backend, cx + radius * std::cos(rad), cy + radius * std::sin(rad),
                   dot_radius, color);
    }
}

}  // namespace

// ---- battery ------------------------------------------------------------------
//
// ADR-0058 second round: the reference battery (round10 b10 x2496-2506) is a
// SOLID white body ~11x30 physical px with a grey cap on top - not the outlined
// pill with an inner level bar this file drew in the first round.
void DrawBatteryGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                      int percent, bool charging, bool alert,
                      const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float cx = slot.CenterX();
    const float cap_w = std::max(2.0f, slot.Width() * 0.42f);
    const float cap_h = std::max(1.2f, slot.Height() * 0.07f);
    const float body_top = slot.top + cap_h * 1.8f;
    const float body_w = std::max(3.0f, slot.Width() * 0.62f);
    const float body_h = slot.bottom - body_top;
    const render::RectF body{cx - body_w * 0.5f, body_top, cx + body_w * 0.5f, slot.bottom};
    const float radius = body_w * 0.30f;

    // Solid body: white at normal charge, the frozen HIG systemRed below 20%.
    const render::ColorF body_color =
        alert ? kWidgetCriticalColor : render::ColorF{color.r, color.g, color.b, 1.0f};
    backend.FillRoundedRect(body, radius, body_color);
    // Grey cap (the reference's "grey hat").
    render::ColorF cap_color = color;
    cap_color.a *= 0.55f;
    backend.FillRoundedRect(render::RectF{cx - cap_w * 0.5f, slot.top, cx + cap_w * 0.5f,
                                          slot.top + cap_h},
                            cap_w * 0.5f, cap_color);

    // Charging bolt: a dark stepped zig-zag ON the solid body (visible against
    // white or red, unlike the old white-on-white bolt).
    if (charging) {
        const float step = body_w * 0.26f;
        const float y0 = body.top + body_h * 0.20f;
        render::ColorF bolt{0.10f, 0.11f, 0.13f, 0.85f};
        FillCircle(backend, cx, y0, step * 0.8f, bolt);
        FillCircle(backend, cx - step * 0.5f, y0 + step * 1.6f, step * 0.8f, bolt);
        FillCircle(backend, cx + step * 0.4f, y0 + step * 3.2f, step * 0.8f, bolt);
    }
}

// ---- wifi / ethernet ----------------------------------------------------------

WifiGlyphGeometry WifiGlyphGeometry::Compute(const render::RectF& slot) {
    WifiGlyphGeometry g;
    // Sized to the reference's 19x16 physical px on the 16 DIP box: the outer arc
    // spans ~9.5 DIP wide and the whole glyph ~8 DIP tall. Overlap stays dense so
    // the bands read solid.
    g.apex_x = slot.CenterX();
    g.apex_y = slot.bottom - slot.Height() * 0.08f;
    g.arc_count = 3;
    g.dot_radius = std::max(0.7f, slot.Width() * 0.06f);
    const float base = slot.Width() * 0.17f;
    const float step = slot.Width() * 0.12f;
    g.radii[0] = base;
    g.radii[1] = base + step;
    g.radii[2] = base + step * 2.0f;
    g.start_deg = -135.0f;
    g.end_deg = -45.0f;
    const float rad_per_deg = 3.14159265358979f / 180.0f;
    const float outer_len = g.radii[2] * (g.end_deg - g.start_deg) * rad_per_deg;
    const float stride = std::max(0.5f, g.dot_radius * 0.85f);
    g.steps_per_arc = std::max(6, static_cast<int>(std::lround(outer_len / stride)));
    return g;
}

void DrawWifiGlyph(render::IRenderBackend& backend, const render::RectF& slot, int bars,
                   const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const WifiGlyphGeometry g = WifiGlyphGeometry::Compute(slot);

    // Apex dot.
    FillCircle(backend, g.apex_x, g.apex_y, g.dot_radius * 1.05f, color);
    for (int arc = 0; arc < g.arc_count; ++arc) {
        const bool lit = arc < std::clamp(bars, 0, 4);
        render::ColorF arc_color = color;
        if (!lit) arc_color.a *= 0.28f;
        FillArcBand(backend, g.apex_x, g.apex_y, g.radii[arc], g.start_deg, g.end_deg,
                    g.dot_radius, arc_color);
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

// ---- notification bell / tray chevron / volume / spotlight (ADR-0058) ---------

void DrawBellGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    // The reference bell.png (round10 x2607-2636): a RED rounded tile with a
    // white bell inside. Tile = the slot's own square, white bell = the glyph
    // colour the bar passes (white on the dark bar).
    backend.FillRoundedRect(slot, std::min(slot.Width(), slot.Height()) * 0.28f,
                            kWidgetCriticalColor);
    const float w = slot.Width();
    const float h = slot.Height();
    const float cx = slot.CenterX();
    render::ColorF white{color.r, color.g, color.b, 1.0f};
    // White bell, sized to ~60% of the tile so the red frame stays visible.
    const float dome_r = w * 0.17f;
    const float dome_cy = slot.top + h * 0.34f;
    const float skirt_top = dome_cy + dome_r * 0.15f;
    const float skirt_h = h * 0.22f;
    const float skirt_w = w * 0.42f;
    backend.FillRoundedRect(render::RectF{cx - skirt_w * 0.5f, skirt_top,
                                          cx + skirt_w * 0.5f, skirt_top + skirt_h},
                            skirt_w * 0.22f, white);
    FillCircle(backend, cx, dome_cy, dome_r, white);
    const float lip_w = w * 0.52f;
    const float lip_h = h * 0.07f;
    backend.FillRoundedRect(render::RectF{cx - lip_w * 0.5f, skirt_top + skirt_h - lip_h * 0.5f,
                                          cx + lip_w * 0.5f, skirt_top + skirt_h + lip_h * 0.5f},
                            lip_h * 0.5f, white);
    FillCircle(backend, cx, slot.bottom - h * 0.14f, w * 0.06f, white);
}

void DrawTrayGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float w = slot.Width();
    const float h = slot.Height();
    const float cx = slot.CenterX();
    // ONE solid caret: two diagonal bands of densely overlapping squares meeting
    // at the top centre. Stride is ~55% of the square so each band reads as one
    // thick stroke, not a dotted mountain.
    const float s = std::max(1.4f, w * 0.20f);
    const float apex_y = slot.top + h * 0.24f;
    const float base_y = slot.bottom - h * 0.30f;
    const int steps = 5;
    for (int i = 0; i < steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float x0 = cx - w * 0.30f + w * 0.24f * t;
        const float y0 = base_y - (base_y - apex_y) * t;
        backend.FillRoundedRect(render::RectF{x0 - s * 0.5f, y0 - s * 0.5f,
                                              x0 + s * 0.5f, y0 + s * 0.5f},
                                s * 0.35f, color);
        const float x1 = cx + w * 0.30f - w * 0.24f * t;
        backend.FillRoundedRect(render::RectF{x1 - s * 0.5f, y0 - s * 0.5f,
                                              x1 + s * 0.5f, y0 + s * 0.5f},
                                s * 0.35f, color);
    }
    // Apex: the two bands meet at the top centre.
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
    // Upright speaker: a boxy body on the left, a symmetric cone flaring right,
    // then TWO arc bands (the reference b5-volume's double wave).
    const float body_cy = slot.CenterY();
    const float body_w = w * 0.14f;
    const float body_h = h * 0.32f;
    const float body_left = cx - w * 0.40f;
    backend.FillRoundedRect(
        render::RectF{body_left, body_cy - body_h * 0.5f, body_left + body_w,
                      body_cy + body_h * 0.5f},
        body_w * 0.15f, color);
    // Cone: symmetric steps widening from the body's midline.
    const float cone_base = body_left + body_w;
    for (int i = 0; i < 2; ++i) {
        const float seg_w = w * 0.14f;
        const float seg_h = body_h * (0.62f + 0.55f * static_cast<float>(i));
        const float x = cone_base + seg_w * static_cast<float>(i);
        backend.FillRoundedRect(
            render::RectF{x, body_cy - seg_h * 0.5f, x + seg_w + 0.6f,
                          body_cy + seg_h * 0.5f},
            1.0f, color);
    }
    // Two wave arcs, centred at the cone mouth.
    const float wave_cx = cone_base + w * 0.14f;
    FillArcBand(backend, wave_cx, body_cy, w * 0.16f, -55.0f, 55.0f,
                std::max(0.7f, w * 0.05f), color);
    FillArcBand(backend, wave_cx, body_cy, w * 0.30f, -55.0f, 55.0f,
                std::max(0.7f, w * 0.05f), color);
}

void DrawSpotlightGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                        const render::ColorF& color) {
    if (slot.Width() <= 0.0f || slot.Height() <= 0.0f) return;
    const float w = slot.Width();
    const float h = slot.Height();
    // Solid thin-line lens ring up-left (dense disc chain at a thin radius reads
    // as a continuous stroke), short stepped handle down-right. Total footprint
    // ~8 DIP - the reference's 16 physical px on the 16 DIP box.
    const float lens_r = std::min(w, h) * 0.16f;
    const float lens_cx = slot.left + w * 0.36f;
    const float lens_cy = slot.top + h * 0.36f;
    const float stroke = std::max(0.8f, w * 0.055f);
    FillArcBand(backend, lens_cx, lens_cy, lens_r, 0.0f, 360.0f, stroke, color);
    const float s = std::max(1.1f, w * 0.11f);
    for (int i = 0; i < 3; ++i) {
        const float t = static_cast<float>(i);
        const float x = lens_cx + lens_r * 0.85f + s * 0.85f * t;
        const float y = lens_cy + lens_r * 0.85f + s * 0.85f * t;
        backend.FillRoundedRect(render::RectF{x - s * 0.5f, y - s * 0.5f, x + s * 0.5f,
                                              y + s * 0.5f},
                                s * 0.35f, color);
    }
}

}  // namespace dockforge::finderbar
