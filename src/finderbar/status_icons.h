// DockForge - the right-region status glyphs (T-306).
//
// ASSETS.md rule, restated: the icons are self-drawn Direct2D geometry, composed
// only from the C-04 backend contract (FillRoundedRect / StrokeRoundedRect - the
// same primitive set dock::DrawWeatherGlyph is built from, dock_view.cpp:122).
// No Apple artwork, no SF Symbols, no image files; the Beautiful round10/round5
// evidence screenshots are measurement references only (the battery outer frame
// reads ~25x12 DIP in the round10 top bar; the WiFi indicator is three arcs).
//
// Every painter derives its geometry from the slot it is given, so the bar's 16
// DIP icon box and the status popup's larger detail rows share one code path and
// can never drift.
#pragma once

#include "render/backend.h"

namespace dockforge::finderbar {

// The battery: an outline body + right-hand cap, an interior level bar, and a bolt
// when charging. `percent` is 0..100; `alert` paints the level (and nothing else)
// in the frozen HIG systemRed, the <20% state the T-306 spec pins. `color` is the
// glyph's body colour - the bar passes its theme glyph colour, the popup its text
// colour.
void DrawBatteryGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                      int percent, bool charging, bool alert,
                      const render::ColorF& color);

// Wi-Fi: the apex dot and the three arcs the ask names, as dot chains along
// circular paths (an arc is not in the C-04 primitive set; a chain of small discs
// is the same trick the weather glyph uses for diagonals). `bars` is 0..4 from
// WifiBarsForQuality; arcs beyond the bar count render dimmed, so strength is
// readable at a glance and a disconnected state is visibly dim rather than empty.
void DrawWifiGlyph(render::IRenderBackend& backend, const render::RectF& slot, int bars,
                   const render::ColorF& color);

// Ethernet: the plug glyph (body outline + pins). Shown when the active adapter is
// wired; same slot contract as the Wi-Fi glyph.
void DrawEthernetGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                       const render::ColorF& color);

// The one Wi-Fi arc geometry the painter and nothing else needs to agree on.
// Exposed pure so the unit tests can pin the dot count per arc without a device.
struct WifiGlyphGeometry {
    float apex_x = 0.0f;
    float apex_y = 0.0f;
    int arc_count = 3;
    int dots_per_arc = 0;   // discs per arc
    float dot_radius = 0.0f;
    float radii[3] = {0.0f, 0.0f, 0.0f};  // innermost first
    float start_deg = 0.0f;               // arc span, degrees; y-down screen space
    float end_deg = 0.0f;

    static WifiGlyphGeometry Compute(const render::RectF& slot);
};

}  // namespace dockforge::finderbar
