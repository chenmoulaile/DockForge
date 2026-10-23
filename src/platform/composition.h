// DockForge - DirectComposition + Direct2D render surface for a layered window.
//
// The object graph is fixed by what T-004 measured on this host (ADR-0007):
//   ID3D11Device -> IDXGIDevice -> ID2D1Device -> DCompositionCreateDevice2
// Passing IDXGIDevice straight to DCompositionCreateDevice2 makes every call
// succeed up to Commit(), then IDCompositionSurface::BeginDraw fails with
// E_NOINTERFACE. So the D2D device is mandatory, not optional.
#pragma once

#include "platform/win_common.h"

#include <d3d11.h>
#include <dxgi1_2.h>

#include <functional>

namespace dockforge::platform {

// One DComp-backed surface for one HWND. Reusable across resizes.
class CompositionSurface {
  public:
    CompositionSurface() = default;
    ~CompositionSurface();

    CompositionSurface(const CompositionSurface&) = delete;
    CompositionSurface& operator=(const CompositionSurface&) = delete;

    // Creates the device stack and binds a visual tree to `hwnd`.
    // `hwnd` must already have WS_EX_NOREDIRECTIONBITMAP (WindowDesc
    // .composition_surface = true), otherwise DWM keeps painting the
    // redirection surface and nothing appears.
    bool Initialize(HWND hwnd);

    // Allocates a new BGRA8 premultiplied surface of the given physical size.
    bool Resize(int width_px, int height_px);

    // Geometry handed to a paint callback. The surface size is NOT the D2D
    // target's GetPixelSize(): BeginDraw can return a non-zero offset into a
    // larger atlas bitmap, so GetPixelSize() reports the atlas, not this
    // surface. Always lay out against these fields.
    struct PaintInfo {
        float dpi = 96.0f;
        float width_dip = 0.0f;
        float height_dip = 0.0f;
    };

    // Runs a paint pass. The callback receives a D2D context already translated
    // to the surface origin, with DPI set, so drawing happens in DIPs.
    using PaintCallback = std::function<void(ID2D1DeviceContext&, const PaintInfo&)>;
    bool Paint(float dpi, const PaintCallback& paint);

    // Pushes the visual tree to DWM. Call once per frame after Paint().
    bool Commit();

    void Shutdown();

    bool IsValid() const { return hwnd_ != nullptr && dcomp_ != nullptr; }
    int WidthPx() const { return width_px_; }
    int HeightPx() const { return height_px_; }

    // Exposed for later milestones that need raw access (e.g. DComp animations).
    ID2D1Device* D2DDevice() const { return d2d_device_.Get(); }
    IDCompositionDesktopDevice* DCompDevice() const { return dcomp_.Get(); }

  private:
    bool CreateDeviceStack();
    bool CreateVisualTree();

    HWND hwnd_ = nullptr;
    int width_px_ = 0;
    int height_px_ = 0;

    ComPtr<ID3D11Device> d3d_;
    ComPtr<IDXGIDevice> dxgi_device_;
    ComPtr<ID2D1Factory1> d2d_factory_;
    ComPtr<ID2D1Device> d2d_device_;
    ComPtr<IDCompositionDesktopDevice> dcomp_;
    ComPtr<IDCompositionTarget> target_;
    ComPtr<IDCompositionVisual2> visual_;
    ComPtr<IDCompositionSurface> surface_;
};

// Process-wide D2D factory. D2D factories are expensive to create and are
// explicitly shareable between devices, so one instance serves every surface.
ID2D1Factory1* SharedD2DFactory();

}  // namespace dockforge::platform
