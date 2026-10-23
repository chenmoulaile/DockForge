// DockForge - material tiers and the self-drawn frosted plate (T-005).
//
// What the spike established, and why this file looks the way it does:
//
//   * Mica and the accent-policy acrylic DO blur on this host, but they paint the
//     entire window rectangle. They ignore the DComp surface's per-pixel alpha,
//     ignore SetWindowRgn, and DWM's own rounded corners use a fixed radius that
//     does not follow appearance.corner_radius. Measured in
//     spikes/t005_material/README.md conclusion 1.
//   * A screen readback costs a flat ~78ms regardless of region size, but only
//     0.5% of that is CPU time (conclusion 3 + capcost part F). It is a blocking
//     compositor round-trip, so it belongs on a worker thread.
//   * With WDA_EXCLUDEFROMCAPTURE set, a capture of the desktop no longer contains
//     our own window, which is what makes a self-drawn plate possible at all
//     (conclusion 2).
//
// So the plate is always drawn by us; the tier selects how much frost it gets and
// whether a system backdrop is used underneath. The system backdrop is only usable
// when the plate is a plain rectangle (corner_radius == 0), because it cannot be
// clipped to a custom radius.
#pragma once

#include "core/config.h"
#include "platform/win_common.h"
#include "render/backend.h"

#include <cstdint>

namespace dockforge::render {

// Everything the tier decision depends on. Split from the probe so the resolver
// stays a pure function and can be unit-tested without a desktop.
struct MaterialCaps {
    int os_build = 0;
    bool system_backdrop_supported = false;
    bool capture_available = false;
    int corner_radius = 22;
};

// Degrades `requested` down to something this machine can actually render.
// Never returns a tier that needs a capability the caps say is missing.
core::MaterialLevel ResolveMaterialTier(core::MaterialLevel requested,
                                        const MaterialCaps& caps);

// Why the resolver moved away from `requested`, or "" when it did not. Logged once
// at startup so a user reporting "my Mica looks like acrylic" has an answer.
const char* DegradeReason(core::MaterialLevel requested, const MaterialCaps& caps);

struct PlateStyle {
    core::MaterialLevel tier = core::MaterialLevel::Solid;
    float blur_sigma = 0.0f;
    float tint_alpha = 0.92f;
    float border_alpha = 0.16f;
    bool use_capture = false;
    bool use_system_backdrop = false;
};

PlateStyle StyleForTier(core::MaterialLevel tier, int corner_radius);

// Reads the OS build from the registry (ProductName lies about Windows 11, the
// build number does not) and decides whether DWMWA_SYSTEMBACKDROP_TYPE can work.
MaterialCaps ProbeMaterialCaps(int corner_radius);

// Owns the backdrop bitmap and the blur effect across frames. Creating either per
// frame costs a shader compile, measured at 620ms for the first frame in the spike.
class PlateRenderer {
  public:
    // `backdrop` is the latest captured BGRA8 top-down frame in PHYSICAL pixels,
    // or null to fall back to a flat plate. `dpi` is the frame DPI: the bitmap is
    // tagged with it so D2D maps its pixels onto physical pixels instead of
    // stretching them by `dpi/96` (a 1.5x-too-large frost at 144 DPI).
    // Returns false only when D2D refused the upload.
    bool Draw(ID2D1DeviceContext& dc, IRenderBackend& backend, const RectF& rect, float radius,
              const PlateStyle& style, const std::uint8_t* backdrop, int backdrop_w,
              int backdrop_h, float dpi);

    void Reset();

  private:
    ComPtr<ID2D1Bitmap1> bitmap_;
    ComPtr<ID2D1Effect> blur_;
    ComPtr<ID2D1RoundedRectangleGeometry> clip_;
    float clip_radius_ = -1.0f;
    RectF clip_rect_{};
    int bitmap_w_ = 0;
    int bitmap_h_ = 0;
    float bitmap_dpi_ = 0.0f;
};

}  // namespace dockforge::render
