// DockForge - DirectComposition surface implementation.
#include "platform/composition.h"

#include "core/log.h"
#include "core/paths.h"

#include <algorithm>

namespace dockforge::platform {
namespace {

// BGRA8 premultiplied is the only format DComp composites with per-pixel alpha.
constexpr DXGI_FORMAT kSurfaceFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

}  // namespace

ID2D1Factory1* SharedD2DFactory() {
    static ComPtr<ID2D1Factory1> factory = [] {
        ComPtr<ID2D1Factory1> created;
        D2D1_FACTORY_OPTIONS options{};
        const HRESULT hr =
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                              reinterpret_cast<void**>(created.GetAddressOf()));
        if (FAILED(hr)) {
            DF_ERROR("render", "D2D1CreateFactory failed: %s",
                     core::WideToUtf8(HResultText(hr)).c_str());
            return ComPtr<ID2D1Factory1>{};
        }
        return created;
    }();
    return factory.Get();
}

CompositionSurface::~CompositionSurface() { Shutdown(); }

bool CompositionSurface::CreateDeviceStack() {
    if (dcomp_) return true;

    ID2D1Factory1* factory = SharedD2DFactory();
    if (!factory) return false;
    d2d_factory_ = factory;

    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                   ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
    if (FAILED(hr)) {
        DF_WARN("render", "hardware D3D11 device unavailable (%s), falling back to WARP",
                core::WideToUtf8(HResultText(hr)).c_str());
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels,
                               ARRAYSIZE(levels), D3D11_SDK_VERSION, &d3d_, nullptr, nullptr);
        if (FAILED(hr)) {
            DF_ERROR("render", "D3D11CreateDevice failed: %s",
                     core::WideToUtf8(HResultText(hr)).c_str());
            return false;
        }
    }

    hr = d3d_.As(&dxgi_device_);
    if (FAILED(hr)) {
        DF_ERROR("render", "ID3D11Device -> IDXGIDevice failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }

    hr = d2d_factory_->CreateDevice(dxgi_device_.Get(), &d2d_device_);
    if (FAILED(hr)) {
        DF_ERROR("render", "ID2D1Factory1::CreateDevice failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }

    // Must be the D2D device: see the header comment and ADR-0007.
    hr = DCompositionCreateDevice2(d2d_device_.Get(), IID_PPV_ARGS(&dcomp_));
    if (FAILED(hr)) {
        DF_ERROR("render", "DCompositionCreateDevice2 failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }
    return true;
}

bool CompositionSurface::CreateVisualTree() {
    if (target_) return true;

    HRESULT hr = dcomp_->CreateTargetForHwnd(hwnd_, TRUE, &target_);
    if (FAILED(hr)) {
        DF_ERROR("render", "CreateTargetForHwnd failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }

    hr = dcomp_->CreateVisual(&visual_);
    if (FAILED(hr)) {
        DF_ERROR("render", "IDCompositionDevice2::CreateVisual failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }

    hr = target_->SetRoot(visual_.Get());
    if (FAILED(hr)) {
        DF_ERROR("render", "IDCompositionTarget::SetRoot failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }
    return true;
}

bool CompositionSurface::Initialize(HWND hwnd) {
    if (!hwnd) return false;
    hwnd_ = hwnd;
    if (!CreateDeviceStack()) return false;
    if (!CreateVisualTree()) return false;
    DF_DEBUG("render", "composition initialized for hwnd=%p", static_cast<void*>(hwnd));
    return true;
}

bool CompositionSurface::Resize(int width_px, int height_px) {
    if (!IsValid()) return false;
    width_px = std::max(width_px, 1);
    height_px = std::max(height_px, 1);
    if (surface_ && width_px == width_px_ && height_px == height_px_) return true;

    ComPtr<IDCompositionSurface> created;
    const HRESULT hr = dcomp_->CreateSurface(static_cast<UINT>(width_px),
                                             static_cast<UINT>(height_px), kSurfaceFormat,
                                             DXGI_ALPHA_MODE_PREMULTIPLIED, &created);
    if (FAILED(hr)) {
        DF_ERROR("render", "IDCompositionDevice2::CreateSurface(%dx%d) failed: %s", width_px,
                 height_px, core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }

    // Swap in the new surface; the visual must point at the live one.
    surface_ = created;
    width_px_ = width_px;
    height_px_ = height_px;

    const HRESULT content_hr = visual_->SetContent(surface_.Get());
    if (FAILED(content_hr)) {
        DF_ERROR("render", "IDCompositionVisual::SetContent failed: %s",
                 core::WideToUtf8(HResultText(content_hr)).c_str());
        return false;
    }
    DF_DEBUG("render", "surface resized to %dx%d px", width_px_, height_px_);
    return true;
}

bool CompositionSurface::Paint(float dpi, const PaintCallback& paint) {
    if (!surface_ || !paint) return false;

    ComPtr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = surface_->BeginDraw(nullptr, IID_PPV_ARGS(&dc), &offset);
    if (FAILED(hr)) {
        DF_ERROR("render", "IDCompositionSurface::BeginDraw failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }

    dc->SetDpi(dpi, dpi);
    // BeginDraw reports the surface's offset INSIDE the atlas bitmap in physical
    // pixels. SetTransform works in DIPs, so the offset must be converted or the
    // content slides by (dpi/96 - 1) * offset on non-96-DPI monitors.
    const float dip_scale = 96.0f / (dpi > 0.0f ? dpi : 96.0f);
    dc->SetTransform(D2D1::Matrix3x2F::Translation(
        static_cast<float>(offset.x) * dip_scale, static_cast<float>(offset.y) * dip_scale));
    // Clear to fully transparent so rounded corners and shadows composite.
    dc->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // BeginDraw hands back a sub-rect of a larger atlas, so the context's
    // GetPixelSize() is the atlas, not this surface. Laying out against it
    // overflows the window: measured at 96 DPI the rounded rect grew past the
    // right edge (corner_tr sampled as painted) and at 144 DPI the centre
    // sampled as transparent. Always derive DIP size from width_px_/dpi.
    PaintInfo info{};
    info.dpi = dpi > 0.0f ? dpi : 96.0f;
    info.width_dip = static_cast<float>(width_px_) * 96.0f / info.dpi;
    info.height_dip = static_cast<float>(height_px_) * 96.0f / info.dpi;

    paint(*dc.Get(), info);

    // Dropping the target before EndDraw is required by D2D.
    dc->SetTarget(nullptr);
    hr = surface_->EndDraw();
    if (FAILED(hr)) {
        DF_ERROR("render", "IDCompositionSurface::EndDraw failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }
    return true;
}

bool CompositionSurface::Commit() {
    if (!dcomp_) return false;
    const HRESULT hr = dcomp_->Commit();
    if (FAILED(hr)) {
        DF_ERROR("render", "IDCompositionDevice2::Commit failed: %s",
                 core::WideToUtf8(HResultText(hr)).c_str());
        return false;
    }
    return true;
}

void CompositionSurface::Shutdown() {
    surface_.Reset();
    visual_.Reset();
    target_.Reset();
    dcomp_.Reset();
    d2d_device_.Reset();
    d2d_factory_.Reset();
    dxgi_device_.Reset();
    d3d_.Reset();
    hwnd_ = nullptr;
    width_px_ = 0;
    height_px_ = 0;
}

}  // namespace dockforge::platform
