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

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
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
        bitmaps_.clear();
        formats_.clear();
        dwrite_.Reset();
        factory_.Reset();
        dc_ = nullptr;
    }

    void BeginFrame(ID2D1DeviceContext& dc, float dpi) override {
        dc_ = &dc;
        dpi_ = dpi > 0.0f ? dpi : 96.0f;
        // Brushes are device-dependent; a new context means a new device generation.
        // Bitmaps are device resources too, so they go with the brushes.
        if (dc_ != last_dc_) {
            brushes_.fill(Entry{});
            bitmaps_.clear();
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

    void DrawBitmap(std::uint64_t cache_id, const BitmapView& view, const RectF& rect,
                    float opacity) override {
        if (!dc_ || !view.Valid() || opacity <= 0.0f) return;
        ID2D1Bitmap1* bitmap = BitmapFor(cache_id, view);
        if (!bitmap) return;
        const D2D1_RECT_F dst = ToD2D(rect);
        // Downscale filter, chosen by rendering all four modes and measuring the output.
        //
        // Dock tiles are extracted at 256px and drawn into a 48 dip slot - 96px at 192
        // DPI, a 2.67x reduction. LINEAR samples a 2x2 neighbourhood, so at that ratio
        // most source texels never reach the destination. Measured on this machine
        // (spikes/T-301_finderbar_menu/interp_check2.cpp, hardware D3D11 target, results
        // in evidence/T-301/interp.txt):
        //
        //   checkerboard 256->96, ideal flat 127 / zero spread
        //     LINEAR              stdev 37.67   <- aliasing, the visible "mush"
        //     MULTI_SAMPLE_LINEAR stdev 19.19
        //     HIGH_QUALITY_CUBIC  stdev  0.48
        //     ANISOTROPIC         stdev  0.01
        //   notepad 256->96 against the shell's own 96px and a CPU box filter (lower rms
        //   is closer; the two references agree with each other at rms 1.469)
        //     LINEAR              rms_shell 5.209  rms_box 5.791
        //     MULTI_SAMPLE_LINEAR rms_shell 2.143  rms_box 1.339
        //     HIGH_QUALITY_CUBIC  rms_shell 2.403  rms_box 1.813
        //     ANISOTROPIC         rms_shell 6.353  rms_box 5.587
        //
        // MULTI_SAMPLE_LINEAR wins the fidelity test outright - it lands closer to a
        // correct area average (1.339) than the shell's own 96px rendition does (1.469) -
        // and it halves the aliasing of LINEAR. HIGH_QUALITY_CUBIC is unbeatable on the
        // synthetic checkerboard but visibly softens a real icon (rms_box 1.813), which
        // is the wrong trade for a dock. ANISOTROPIC only wins where there is no detail
        // to preserve. So: multi-sample linear.
        dc_->DrawBitmap(bitmap, &dst, std::clamp(opacity, 0.0f, 1.0f),
                        D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR, nullptr, nullptr);
    }

  private:
    struct Entry {
        ColorF color{};
        ComPtr<ID2D1SolidColorBrush> brush;
        bool used = false;
    };

    // One GPU bitmap per caller-assigned content id. See the contract note in
    // backend.h for why the key is an id and not a pointer.
    struct BitmapEntry {
        ComPtr<ID2D1Bitmap1> bitmap;
        int width = 0;
        int height = 0;
    };

    static D2D1_RECT_F ToD2D(const RectF& r) {
        return D2D1::RectF(r.left, r.top, r.right, r.bottom);
    }

    ID2D1Bitmap1* BitmapFor(std::uint64_t cache_id, const BitmapView& view) {
        auto it = bitmaps_.find(cache_id);
        if (it != bitmaps_.end()) {
            BitmapEntry& entry = it->second;
            if (entry.width == view.width && entry.height == view.height && entry.bitmap) {
                return entry.bitmap.Get();
            }
            // Same id, different size: the caller reused an id for new content, so
            // the old texture is stale. Drop it and rebuild below.
            bitmaps_.erase(it);
        }

        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(view.width),
                                             static_cast<UINT32>(view.height));
        // DPI must be 96 here: the destination rect is already in DIP and the
        // backend's transform carries the frame DPI. Tagging the bitmap with the
        // frame DPI would scale it a second time (the T-005 frost bitmap bug).
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f,
            96.0f);
        BitmapEntry entry;
        entry.width = view.width;
        entry.height = view.height;
        const UINT32 pitch = static_cast<UINT32>(view.width) * 4u;
        const HRESULT hr =
            dc_->CreateBitmap(size, view.pixels, pitch, props, entry.bitmap.GetAddressOf());
        if (FAILED(hr)) {
            DF_WARN("render", "CreateBitmap(%dx%d) failed: %s", view.width, view.height,
                    core::WideToUtf8(HResultText(hr)).c_str());
            return nullptr;
        }
        const auto [pos, inserted] = bitmaps_.emplace(cache_id, std::move(entry));
        return inserted ? pos->second.bitmap.Get() : nullptr;
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
    std::unordered_map<std::uint64_t, BitmapEntry> bitmaps_;
};

}  // namespace

std::unique_ptr<IRenderBackend> CreateD2DBackend() {
    return std::make_unique<D2DBackend>();
}

}  // namespace dockforge::render
