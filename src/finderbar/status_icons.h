// DockForge - the right-region status glyphs (T-306).
//
// ASSETS.md rule, restated: the icons are self-drawn Direct2D geometry, composed
// only from the C-04 backend contract (FillRoundedRect / StrokeRoundedRect - the
// same primitive set dock::DrawWeatherGlyph is built from, dock_view.cpp:122).
// No Apple artwork, no SF Symbols, no image files; the Beautiful round10/round5
// evidence screenshots are measurement references only.
//
// ADR-0058 second round pins every glyph's size to the reference's own pixel
// measurements (the first round's shapes were right, the SCALE and style were
// not): battery = SOLID white body with a grey cap, ~11x30 physical px; wifi =
// solid bands, 19x16 px; spotlight = a solid thin-line magnifier, 16 px; tray =
// ONE solid chevron, not a dotted mountain; bell = the reference bell.png's RED
// rounded tile with a white bell inside; volume = an upright speaker with TWO
// wave arcs, ~32 px wide.
//
// Every painter derives its geometry from the slot it is given, so the bar's 16
// DIP icon box and the status popup's larger detail rows share one code path and
// can never drift.
#pragma once

#include "render/backend.h"

namespace dockforge::finderbar {

// The battery: a SOLID body (the reference's full-charge white card) with a cap on
// top, a RED body when `alert` (<20%, the frozen HIG systemRed), and a dark bolt
// when charging. `percent` drives nothing but the alert flag's threshold today -
// the reference's solid card does not show a level bar.
void DrawBatteryGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                      int percent, bool charging, bool alert,
                      const render::ColorF& color);

// Wi-Fi: the apex dot and the three arcs as SOLID bands sized to the reference's
// 19x16 physical px (the first round drew them 1.5x too large). `bars` is 0..4
// from WifiBarsForQuality; arcs beyond the bar count render dimmed.
void DrawWifiGlyph(render::IRenderBackend& backend, const render::RectF& slot, int bars,
                   const render::ColorF& color);

// Ethernet: the plug glyph (body outline + pins). Shown when the active adapter is
// wired; same slot contract as the Wi-Fi glyph.
void DrawEthernetGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                       const render::ColorF& color);

// The notification bell: the reference bell.png's RED rounded tile with a WHITE
// bell inside (round10 x2607-2636).
void DrawBellGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color);

// The tray chevron (tray_mirror, C-31 enum): ONE solid upward caret built from
// densely overlapping squares (the first round's two dotted mountain layers read
// wrong). Marks the collapsed overflow.
void DrawTrayGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color);

// The volume speaker (volume, C-31 enum extended by ADR-0058): an UPRIGHT solid
// speaker with TWO sound-wave arc bands, sized to the reference's ~32 px width.
// The reference fixture shows the muted form (speaker + slash); this painter
// draws the unmuted form because it is the only state the bar can honestly claim
// without an audio read on its Tick chain - the mute slash belongs to the control
// centre, which owns the audio state.
void DrawVolumeGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                     const render::ColorF& color);

// The spotlight magnifier (spotlight, C-31 enum extended by ADR-0058): a solid
// thin-line lens ring and a short handle, sized to the reference's 16 px (the
// first round drew a dotted ring 2x too large).
void DrawSpotlightGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                        const render::ColorF& color);

// The one Wi-Fi arc geometry the painter and nothing else needs to agree on.
// Exposed pure so the unit tests can pin the band count per arc without a device.
struct WifiGlyphGeometry {
    float apex_x = 0.0f;
    float apex_y = 0.0f;
    int arc_count = 3;
    int steps_per_arc = 0;  // overlapping discs per arc band
    float dot_radius = 0.0f;
    float radii[3] = {0.0f, 0.0f, 0.0f};  // innermost first
    float start_deg = 0.0f;               // arc span, degrees; y-down screen space
    float end_deg = 0.0f;

    static WifiGlyphGeometry Compute(const render::RectF& slot);
};

}  // namespace dockforge::finderbar
