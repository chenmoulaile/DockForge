// DockForge - Direct2D implementation of IRenderBackend (C-04).
//
// Brushes are cached by colour. The T-004 placeholder rebuilt two brushes per
// frame, which is invisible at 60 FPS for a static panel but becomes a per-frame
// allocation storm once the Dock animates: every frame would create, use and
// destroy three COM objects while the GPU waits. A 16-entry cache is enough for
// the Dock's palette and turns that into a map lookup.
#include "render/backend.h"

#include "core/log.h"
#include "core/paths.h"

#include <d2d1helper.h>
#include <dwrite.h>

#include <array>
#include <cstring>
#include <utility>
#include <vector>

namespace dockforge::render {
namespace {

// Text formats are keyed by (size, weight). The Dock uses one size today, but the
// settings window and Launchpad will not, and creating a format per draw call is
// the same mistake as the brush one.
struct FormatKey {
    float size;
    int weight;

    bool operator==(const FormatKey& other) const {
        return size == other.size && weight == other.weight;
    }
};

class D2DBackend final : public IRenderBackend {
  public:
    bool Initialize() override {
        if (factory_) return true;
        D2D1_FACTORY_OPTIONS options{};
        const HRESULT hr =
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                              &options, reinterpret_cast<void**>(factory_.GetAddressOf()));
        if (FAILED(hr)) {
            DF_ERROR("render", "D2D1CreateFactory failed: %s",
                     core::WideToUtf8(HResultText(hr)).c_str());
            return false;
        }

        HRESULT hr_write = DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
        if (FAILED(hr_write)) {
            DF_ERROR("render", "DWriteCreateFactory failed: %s",
                     core::WideToUtf8(HResultText(hr_write)).c_str());
            return false;
        }
        return true;
    }

    void Shutdown() override {
        brushes_.fill(Entry{});
        formats_.clear();
        dwrite_.Reset();
        factory_.Reset();
        dc_ = nullptr;
    }

    void BeginFrame(ID2D1DeviceContext& dc, float dpi) override {
        dc_ = &dc;
        dpi_ = dpi > 0.0f ? dpi : 96.0f;
        // Brushes are device-dependent; a new context means a new device generation.
        if (dc_ != last_dc_) {
            brushes_.fill(Entry{});
            last_dc_ = dc_;
        }
    }

    void EndFrame() override { dc_ = nullptr; }

    void FillRoundedRect(const RectF& rect, float radius, const ColorF& color) override {
        if (!dc_ || color.a <= 0.0f) return;
        ID2D1Brush* brush = Brush(color);
        if (!brush) return;
        const D2D1_ROUNDED_RECT rr{ToD2D(rect), radius, radius};
        dc_->FillRoundedRectangle(&rr, brush);
    }

    void StrokeRoundedRect(const RectF& rect, float radius, const ColorF& color,
                           float width) override {
        if (!dc_ || color.a <= 0.0f) return;
        ID2D1Brush* brush = Brush(color);
        if (!brush) return;
        const D2D1_ROUNDED_RECT rr{ToD2D(rect), radius, radius};
        dc_->DrawRoundedRectangle(&rr, brush, width);
    }

    void DrawLabel(const wchar_t* text, const RectF& rect, float size_dip, const ColorF& color,
                   TextAlign align) override {
        if (!dc_ || !text || !*text || color.a <= 0.0f) return;
        IDWriteTextFormat* format = Format(size_dip, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        if (!format) return;
        ID2D1Brush* brush = Brush(color);
        if (!brush) return;

        switch (align) {
            case TextAlign::Center:
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                break;
            case TextAlign::Right:
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
                break;
            case TextAlign::Left:
            default:
                format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                break;
        }
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

        (dc_->DrawText)(text, static_cast<UINT32>(wcslen(text)), format, ToD2D(rect), brush,
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }

  private:
    struct Entry {
        ColorF color{};
        ComPtr<ID2D1SolidColorBrush> brush;
        bool used = false;
    };

    static D2D1_RECT_F ToD2D(const RectF& r) {
        return D2D1::RectF(r.left, r.top, r.right, r.bottom);
    }

    ID2D1Brush* Brush(const ColorF& color) {
        for (auto& entry : brushes_) {
            if (entry.used && entry.color.r == color.r && entry.color.g == color.g &&
                entry.color.b == color.b && entry.color.a == color.a) {
                return entry.brush.Get();
            }
        }
        for (auto& entry : brushes_) {
            if (entry.used) continue;
            const D2D1_COLOR_F c = D2D1::ColorF(color.r, color.g, color.b, color.a);
            if (FAILED(dc_->CreateSolidColorBrush(c, entry.brush.GetAddressOf()))) return nullptr;
            entry.color = color;
            entry.used = true;
            return entry.brush.Get();
        }
        // Cache full: draw uncached rather than evicting, because eviction churn
        // under a wider palette is worse than one transient allocation.
        ComPtr<ID2D1SolidColorBrush> temp;
        const D2D1_COLOR_F c = D2D1::ColorF(color.r, color.g, color.b, color.a);
        if (FAILED(dc_->CreateSolidColorBrush(c, temp.GetAddressOf()))) return nullptr;
        spill_ = temp;
        return spill_.Get();
    }

    IDWriteTextFormat* Format(float size, int weight) {
        if (!dwrite_) return nullptr;
        for (auto& [key, format] : formats_) {
            if (key.size == size && key.weight == weight) return format.Get();
        }
        if (formats_.size() >= 8) formats_.clear();

        ComPtr<IDWriteTextFormat> created;
        HRESULT hr = dwrite_->CreateTextFormat(
            L"Segoe UI Variable Display", nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &created);
        if (FAILED(hr)) {
            // Windows 10 has no variable family; Segoe UI is always present.
            hr = dwrite_->CreateTextFormat(L"Segoe UI", nullptr,
                                           static_cast<DWRITE_FONT_WEIGHT>(weight),
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                           size, L"zh-CN", &created);
        }
        if (FAILED(hr)) return nullptr;
        formats_.push_back({FormatKey{size, weight}, created});
        return formats_.back().second.Get();
    }

    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ID2D1DeviceContext* dc_ = nullptr;
    ID2D1DeviceContext* last_dc_ = nullptr;
    float dpi_ = 96.0f;
    std::array<Entry, 16> brushes_{};
    ComPtr<ID2D1SolidColorBrush> spill_;
    std::vector<std::pair<FormatKey, ComPtr<IDWriteTextFormat>>> formats_;
};

}  // namespace

std::unique_ptr<IRenderBackend> CreateD2DBackend() {
    return std::make_unique<D2DBackend>();
}

}  // namespace dockforge::render
