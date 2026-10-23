// DockForge - Dock tile model, geometry and painter (T-009).
//
// Split from main.cpp because the Dock is the product: the host window is
// scaffolding around it. Everything here is expressed in DIPs at the boundary and
// in device pixels inside, which is the only way the DPI matrix stays honest - the
// layout contract (C-05) is defined in device pixels, the render backend (C-04)
// takes DIPs, and the conversion has to happen exactly once.
#pragma once

#include "core/config.h"
#include "dock/layout.h"
#include "platform/win_common.h"
#include "render/backend.h"
#include "render/material.h"
#include "shell/app_entry.h"
#include "shell/app_resolver.h"
#include "shell/icon_cache.h"

#include <cstdint>
#include <string>
#include <vector>

namespace dockforge::dock {

// Breathing room between the icon row and the plate edge, in DIP.
//
// Deliberately not a config key: it is a design constant, not a preference, and
// adding it to C-01 would create a field nobody can meaningfully tune while making
// every theme override longer.
inline constexpr float kPlatePaddingDip = 10.0f;

// One dock slot. The icon is kept by value so painting never touches the shell or
// the cache: a frame that blocks on GetImage is a 60-190ms stall (evidence/T-006).
struct DockTile {
    shell::AppEntry app;
    shell::IconBitmap icon;
    // Stable identity for the render backend's GPU bitmap cache. Derived from the
    // icon source rather than the tile index so reordering the dock does not
    // invalidate every texture.
    std::uint64_t icon_key = 0;
};

// FNV-1a over the UTF-16 source string. Only needs to be collision-resistant
// within one dock's tile list, which is at most a few dozen entries.
std::uint64_t IconKeyFor(const std::wstring& source);

// Deterministic tile list: the AppsFolder scan is already sorted by display name,
// so taking the first `max_tiles` keeps the dock stable across runs instead of
// reshuffling whenever enumeration order changes.
std::vector<DockTile> BuildTiles(const std::vector<shell::AppEntry>& apps, shell::IconCache& icons,
                                 int max_tiles, int icon_px);

struct DockGeometry {
    LayoutParams params{};  // device pixels
    float scale = 1.0f;
    int tile_count = 0;
    float content_width_dip = 0.0f;
    float plate_width_dip = 0.0f;
    float plate_height_dip = 0.0f;
    float window_width_dip = 0.0f;
    float window_height_dip = 0.0f;
};

// Pure function of config + tile count + scale, so the window size can be asserted
// in a test without creating a window.
DockGeometry ComputeGeometry(const core::Config& config, int tile_count, float scale);

// Pointer state in dock-local DIPs. Inactive for the static dock (T-009); T-010
// feeds it from WM_MOUSEMOVE.
struct DockHover {
    bool active = false;
    float mouse_x_dip = 0.0f;
};

struct DockPaintInput {
    const core::Config* config = nullptr;
    const render::PlateStyle* style = nullptr;
    const std::vector<DockTile>* tiles = nullptr;
    const std::uint8_t* backdrop = nullptr;
    int backdrop_w = 0;
    int backdrop_h = 0;
    float dpi = 96.0f;
    DockHover hover{};
};

// Owns the per-frame scratch layout so painting does not allocate. A Dock is
// repainted on every hover frame, and C-05a rule 3 forbids heap traffic there.
class DockPainter {
  public:
    bool Paint(ID2D1DeviceContext& dc, render::IRenderBackend& backend,
               render::PlateRenderer& plate, const DockPaintInput& input);

    const LayoutSummary& LastLayout() const { return last_; }

  private:
    std::vector<IconLayout> scratch_;
    LayoutSummary last_{};
};

}  // namespace dockforge::dock
