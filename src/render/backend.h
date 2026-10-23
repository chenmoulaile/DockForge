// DockForge - drawing backend contract (CONTRACTS.md C-04).
//
// Every coordinate here is a DIP. The backend multiplies by the frame DPI
// internally, so a caller that pre-scales produces double-scaled output at 150%
// and 200% and correct output at 100% - the exact bug shape T-004 already hit
// once with the DComp surface offset.
//
// The text method is called DrawLabel, not DrawText: <winuser.h> defines DrawText
// as an A/W macro, so a member with that name is textually rewritten to DrawTextW
// and every call site breaks in a way that reads like a type error.
#pragma once

#include "platform/win_common.h"

#include <memory>

namespace dockforge::render {

struct ColorF {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct RectF {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float Width() const { return right - left; }
    float Height() const { return bottom - top; }
    float CenterX() const { return (left + right) * 0.5f; }
    float CenterY() const { return (top + bottom) * 0.5f; }
};

enum class TextAlign { Left, Center, Right };

class IRenderBackend {
  public:
    virtual ~IRenderBackend() = default;

    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;

    // Binds one frame. `dc` must already be translated to the surface origin and
    // have its DPI set; platform::CompositionSurface::Paint does both.
    virtual void BeginFrame(ID2D1DeviceContext& dc, float dpi) = 0;
    virtual void EndFrame() = 0;

    virtual void FillRoundedRect(const RectF& rect, float radius, const ColorF& color) = 0;
    virtual void StrokeRoundedRect(const RectF& rect, float radius, const ColorF& color,
                                   float width) = 0;
    virtual void DrawLabel(const wchar_t* text, const RectF& rect, float size_dip,
                           const ColorF& color, TextAlign align) = 0;
};

// The only backend that exists today. Named as a factory so that adding a D3D11
// or software path later does not change a single call site.
std::unique_ptr<IRenderBackend> CreateD2DBackend();

}  // namespace dockforge::render
