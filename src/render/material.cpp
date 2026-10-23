// DockForge - material tier resolution and the self-drawn frosted plate (T-005).
#include "render/material.h"

#include "core/log.h"
#include "core/paths.h"

#include <d2d1effects.h>
#include <d2d1helper.h>

#include <cmath>

namespace dockforge::render {
namespace {

// Tier order from most to least capable. Resolution walks down this list, so
// "degrade" is always a suffix of it and can never produce a tier that ranks above
// what the user asked for.
constexpr core::MaterialLevel kTierOrder[] = {
    core::MaterialLevel::Mica,
    core::MaterialLevel::Acrylic,
    core::MaterialLevel::Blur,
    core::MaterialLevel::Solid,
};

int TierRank(core::MaterialLevel level) {
    for (int i = 0; i < static_cast<int>(std::size(kTierOrder)); ++i) {
        if (kTierOrder[i] == level) return i;
    }
    return static_cast<int>(std::size(kTierOrder)) - 1;
}

// Can `level` actually be rendered with `caps`?
bool TierSupported(core::MaterialLevel level, const MaterialCaps& caps) {
    switch (level) {
        case core::MaterialLevel::Mica:
            // Mica needs Win11 22000+ and a rectangular plate: the system backdrop
            // cannot be clipped to appearance.corner_radius (spike conclusion 1).
            return caps.system_backdrop_supported && caps.os_build >= 22000 &&
                   caps.corner_radius == 0;
        case core::MaterialLevel::Acrylic:
            // Same constraint: the accent-policy acrylic fills the whole window
            // rect and ignores both the surface alpha and SetWindowRgn.
            return caps.corner_radius == 0;
        case core::MaterialLevel::Blur:
            // Self-drawn frost needs a backdrop capture. Without one the plate is
            // still drawable, just flat - that is the Solid tier, so do not claim
            // Blur.
            return caps.capture_available;
        case core::MaterialLevel::Solid:
            return true;
    }
    return false;
}

int ReadOsBuildFromRegistry() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return 0;
    }
    auto guard = MakeGuard([&key] { RegCloseKey(key); });

    // CurrentBuildNumber is a string, not a DWORD. BuildLabEx also carries the
    // revision but its format is not contractual; the plain build number is.
    wchar_t buffer[32]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buffer), &size);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return 0;
    return _wtoi(buffer);
}

}  // namespace

core::MaterialLevel ResolveMaterialTier(core::MaterialLevel requested,
                                        const MaterialCaps& caps) {
    int rank = TierRank(requested);
    while (rank < static_cast<int>(std::size(kTierOrder)) &&
           !TierSupported(kTierOrder[rank], caps)) {
        ++rank;
    }
    if (rank >= static_cast<int>(std::size(kTierOrder))) return core::MaterialLevel::Solid;
    return kTierOrder[rank];
}

const char* DegradeReason(core::MaterialLevel requested, const MaterialCaps& caps) {
    const core::MaterialLevel resolved = ResolveMaterialTier(requested, caps);
    if (resolved == requested) return "";

    // Name the single binding constraint rather than listing every failed check:
    // the first one that fails is what the user can act on.
    switch (requested) {
        case core::MaterialLevel::Mica:
            if (caps.corner_radius != 0) {
                return "system backdrop cannot be clipped to a custom corner radius";
            }
            if (!caps.system_backdrop_supported) {
                return "DWMWA_SYSTEMBACKDROP_TYPE unavailable on this build";
            }
            return "Mica requires Windows 11 build 22000 or later";
        case core::MaterialLevel::Acrylic:
            return "accent-policy acrylic cannot be clipped to a custom corner radius";
        case core::MaterialLevel::Blur:
            return "desktop capture unavailable; frosted plate degrades to solid";
        case core::MaterialLevel::Solid:
            break;
    }
    return "no supported tier";
}

PlateStyle StyleForTier(core::MaterialLevel tier, int corner_radius) {
    PlateStyle style;
    style.tier = tier;
    switch (tier) {
        case core::MaterialLevel::Mica:
            // A system backdrop sits behind a nearly transparent plate; drawing a
            // heavy tint on top would hide the effect the tier exists for.
            style.blur_sigma = 0.0f;
            style.tint_alpha = 0.10f;
            style.border_alpha = 0.12f;
            style.use_capture = false;
            style.use_system_backdrop = true;
            break;
        case core::MaterialLevel::Acrylic:
            style.blur_sigma = 0.0f;
            style.tint_alpha = 0.55f;
            style.border_alpha = 0.16f;
            style.use_capture = false;
            style.use_system_backdrop = true;
            break;
        case core::MaterialLevel::Blur:
            // Sigma is in DIPs; the plate is small so a large sigma would erase all
            // structure. 18 measured as "clearly frosted, still readable" in the
            // spike at Dock scale.
            style.blur_sigma = 18.0f;
            style.tint_alpha = 0.55f;
            style.border_alpha = 0.18f;
            style.use_capture = true;
            style.use_system_backdrop = false;
            break;
        case core::MaterialLevel::Solid:
            style.blur_sigma = 0.0f;
            style.tint_alpha = 0.92f;
            style.border_alpha = 0.16f;
            style.use_capture = false;
            style.use_system_backdrop = false;
            break;
    }
    // A rectangular plate can keep the system backdrop; anything with a custom
    // radius cannot, and StyleForTier must agree with TierSupported.
    if (corner_radius != 0) style.use_system_backdrop = false;
    return style;
}

MaterialCaps ProbeMaterialCaps(int corner_radius) {
    MaterialCaps caps;
    caps.corner_radius = corner_radius;
    caps.os_build = ReadOsBuildFromRegistry();
    // DWMWA_SYSTEMBACKDROP_TYPE (attribute 38) exists from Windows 11 22000. The
    // API entry point always exists on Win10, so the call cannot be used to probe;
    // the build number is the only reliable signal.
    caps.system_backdrop_supported = caps.os_build >= 22000;

    // Capture availability is a property of the machine, not of the API surface:
    // BitBlt against the screen DC is universally available, so the only way it
    // can be missing is a null screen DC (session teardown, secure desktop).
    HDC screen = GetDC(nullptr);
    caps.capture_available = screen != nullptr;
    if (screen) ReleaseDC(nullptr, screen);
    return caps;
}

bool PlateRenderer::Draw(ID2D1DeviceContext& dc, IRenderBackend& backend, const RectF& rect,
                         float radius, const PlateStyle& style, const std::uint8_t* backdrop,
                         int backdrop_w, int backdrop_h, float dpi) {
    if (rect.Width() <= 0.0f || rect.Height() <= 0.0f) return false;

    const float safe_dpi = dpi > 0.0f ? dpi : 96.0f;

    const bool want_blur = style.use_capture && backdrop != nullptr && style.blur_sigma > 0.0f &&
                           backdrop_w > 0 && backdrop_h > 0;
    if (want_blur) {
        if (!bitmap_ || bitmap_w_ != backdrop_w || bitmap_h_ != backdrop_h ||
            bitmap_dpi_ != safe_dpi) {
            bitmap_.Reset();
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), safe_dpi,
                safe_dpi);
            const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(backdrop_w),
                                                 static_cast<UINT32>(backdrop_h));
            if (FAILED(dc.CreateBitmap(size, nullptr, 0, props, &bitmap_))) {
                bitmap_.Reset();
                return false;
            }
            bitmap_w_ = backdrop_w;
            bitmap_h_ = backdrop_h;
            bitmap_dpi_ = safe_dpi;
        }
        const D2D1_RECT_U dirty = D2D1::RectU(0, 0, static_cast<UINT32>(backdrop_w),
                                              static_cast<UINT32>(backdrop_h));
        if (FAILED(bitmap_->CopyFromMemory(&dirty, backdrop,
                                           static_cast<UINT32>(backdrop_w * 4)))) {
            return false;
        }
        if (!blur_) {
            if (FAILED(dc.CreateEffect(CLSID_D2D1GaussianBlur, &blur_))) {
                blur_.Reset();
                return false;
            }
            blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        }
        blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, style.blur_sigma);
        blur_->SetInput(0, bitmap_.Get());
    }

    // Rebuild the clip geometry only when the plate actually changed shape;
    // recreating it per frame is a geometry tessellation on the CPU.
    if (!clip_ || clip_radius_ != radius || clip_rect_.left != rect.left ||
        clip_rect_.top != rect.top || clip_rect_.right != rect.right ||
        clip_rect_.bottom != rect.bottom) {
        clip_.Reset();
        ID2D1Factory* factory = nullptr;
        dc.GetFactory(&factory);
        if (factory) {
            const D2D1_ROUNDED_RECT rr{D2D1::RectF(rect.left, rect.top, rect.right, rect.bottom),
                                       radius, radius};
            factory->CreateRoundedRectangleGeometry(rr, &clip_);
            factory->Release();
        }
        clip_radius_ = radius;
        clip_rect_ = rect;
    }

    ComPtr<ID2D1Layer> layer;
    if (clip_) {
        dc.CreateLayer(nullptr, &layer);
        if (layer) {
            const D2D1_LAYER_PARAMETERS1 params =
                D2D1::LayerParameters1(D2D1::InfiniteRect(), clip_.Get());
            dc.PushLayer(params, layer.Get());
        }
    }

    if (want_blur && blur_) {
        // Tagging the bitmap with the frame DPI makes D2D map its physical pixels
        // onto DIPs by itself, and makes the effect's sigma a DIP value (18 DIP =
        // 27 px at 144 DPI), so the frost looks the same thickness at every scale
        // instead of getting thinner as DPI rises.
        //
        // The src/dst overload of DrawImage was not verified in the spike, so this
        // uses the same point-only form that was measured there, with the plate
        // origin carried in the transform.
        //
        // The transform is COMPOSED with whatever the caller already set rather
        // than overwritten: IDCompositionSurface::BeginDraw returns an atlas
        // offset carried in that transform, and replacing it slides the plate off
        // its own surface (measured: painted=0 at 96 and 144 DPI, correct at 192
        // only because that surface happened to sit at atlas offset zero).
        D2D1_MATRIX_3X2_F base{};
        dc.GetTransform(&base);
        dc.SetTransform(D2D1::Matrix3x2F::Translation(rect.left, rect.top) * base);
        dc.DrawImage(blur_.Get(), D2D1::Point2F(0.0f, 0.0f));
        dc.SetTransform(base);
    }

    backend.FillRoundedRect(rect, radius, ColorF{0.055f, 0.063f, 0.078f, style.tint_alpha});

    if (layer) dc.PopLayer();

    backend.StrokeRoundedRect(RectF{rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f,
                                    rect.bottom - 0.5f},
                              radius, ColorF{1.0f, 1.0f, 1.0f, style.border_alpha}, 1.0f);
    return true;
}

void PlateRenderer::Reset() {
    bitmap_.Reset();
    blur_.Reset();
    clip_.Reset();
    clip_radius_ = -1.0f;
    clip_rect_ = RectF{};
    bitmap_w_ = 0;
    bitmap_h_ = 0;
    bitmap_dpi_ = 0.0f;
}

}  // namespace dockforge::render
