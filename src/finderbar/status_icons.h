// DockForge - the right-region status glyphs (T-306).
//
// ASSETS.md rule, restated: the icons are self-drawn Direct2D geometry, composed
// only from the C-04 backend contract (FillRoundedRect / StrokeRoundedRect - the
// same primitive set dock::DrawWeatherGlyph is built from, dock_view.cpp:122).
// No Apple artwork, no SF Symbols, no image files; the Beautiful round10/round5
// evidence screenshots are measurement references only.
//
// ADR-0058 redraws the battery and the Wi-Fi arcs against the round10 pixel
// scan the fidelity acceptance recorded: the reference battery is
// VERTICAL (a ~10x30 physical px outlined body with a cap on TOP, not the
// horizontal 25x12 pill this file used to draw), and the reference WiFi arcs are
// SOLID strokes (dense overlapping discs), not the dotted chains the first
// renderer used.
//
// Every painter derives its geometry from the slot it is given, so the bar's 16
// DIP icon box and the status popup's larger detail rows share one code path and
// can never drift.
#pragma once

#include "render/backend.h"

namespace dockforge::finderbar {

// The battery: a VERTICAL outline body with a cap on top, an interior level bar
// that fills from the bottom, and a bolt when charging. `percent` is 0..100;
// `alert` paints the level (and nothing else) in the frozen HIG systemRed, the
// <20% state the T-306 spec pins. `color` is the glyph's body colour - the bar
// passes its theme glyph colour, the popup its text colour.
void DrawBatteryGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                      int percent, bool charging, bool alert,
                      const render::ColorF& color);

// Wi-Fi: the apex dot and the three arcs the ask names, drawn as SOLID arc bands
// (dense overlapping discs along circular paths - an arc is not in the C-04
// primitive set, and the acceptance measurement says the reference arcs read as
// continuous strokes, not dot chains). `bars` is 0..4 from WifiBarsForQuality;
// arcs beyond the bar count render dimmed, so strength is readable at a glance
// and a disconnected state is visibly dim rather than empty.
void DrawWifiGlyph(render::IRenderBackend& backend, const render::RectF& slot, int bars,
                   const render::ColorF& color);

// Ethernet: the plug glyph (body outline + pins). Shown when the active adapter is
// wired; same slot contract as the Wi-Fi glyph.
void DrawEthernetGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                       const render::ColorF& color);

// The notification bell (notification_center, C-31 enum): a bell dome over a
// base bar, with the clapper dot beneath. Resident right-cluster item per the
// acceptance reference (round10 x2356-2765 keeps a bell among the four fixtures).
void DrawBellGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color);

// The tray chevron (tray_mirror, C-31 enum): an upward-pointing caret built from
// stepped squares (the same stepped-diagonal trick the weather thunder glyph
// uses - no rotated primitive exists in the C-04 set). Marks the collapsed
// overflow the reference shows as the ^ fixture.
void DrawTrayGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                   const render::ColorF& color);

// The volume speaker (volume, C-31 enum extended by ADR-0058): a solid speaker
// body with one sound-wave arc. The reference fixture shows the muted form
// (speaker + slash); this painter draws the unmuted form because it is the only
// state the bar can honestly claim without an audio read on its Tick chain - the
// mute slash belongs to the control centre, which owns the audio state.
void DrawVolumeGlyph(render::IRenderBackend& backend, const render::RectF& slot,
                     const render::ColorF& color);

// The spotlight magnifier (spotlight, C-31 enum extended by ADR-0058): a ring and
// a stepped-square handle, the launcher fixture.
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
