// DockForge - icon extraction with memory and disk caching (CONTRACTS.md C-08).
#include "shell/icon_cache.h"

#include <windows.h>
#include <wincodec.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>

#include "core/frame_clock.h"
#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"

using Microsoft::WRL::ComPtr;

namespace dockforge::shell {
namespace {

// HBITMAPs from the shell are 32bpp DIBs whose alpha is already premultiplied,
// which is exactly what Direct2D wants. PNG, on the other hand, stores straight
// alpha, so the disk round trip has to unpremultiply on write and let WIC
// premultiply again on read.
void Unpremultiply(std::vector<std::uint8_t>& bgra) {
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const unsigned a = bgra[i + 3];
        if (a == 0 || a == 255) {
            if (a == 0) {
                bgra[i] = 0;
                bgra[i + 1] = 0;
                bgra[i + 2] = 0;
            }
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            const unsigned v = bgra[i + c];
            const unsigned scaled = (v * 255u + a / 2u) / a;
            bgra[i + c] = static_cast<std::uint8_t>(std::min(scaled, 255u));
        }
    }
}

ComPtr<IWICImagingFactory> CreateWicFactory() {
    ComPtr<IWICImagingFactory> factory;
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        DF_WARN("shell", "CoCreateInstance(CLSID_WICImagingFactory) failed: 0x%08lX",
                static_cast<unsigned long>(hr));
        return nullptr;
    }
    return factory;
}

}  // namespace

// Copies an HBITMAP into a tightly packed, top-down BGRA buffer.
//
// The row-order trap that bit us (user-visible: every shell icon drawn upside down):
// IShellItemImageFactory::GetImage hands back a 32bpp BOTTOM-UP DIB section (measured
// with spikes/icon_flip_probe.cpp: dsBmih.biHeight=+192 for cmd.exe). The tempting
// shortcut of memcpy-ing bmBits rows in stored order bakes the flip in - and
// SaveToDisk then persists it into the PNG cache, making it permanent. The repair
// deliberately does NOT branch on bmBits either: GetObjectW's DIBSECTION header
// reported a negative-height section with a positive biHeight on this machine (the
// top-down flip unit test caught that double flip), so the sign cannot be trusted
// from there. GetDIBits with a negative height is the one path whose output order is
// guaranteed by the API contract regardless of the source's row order; its copy cost
// (~0.1ms) is noise next to the ~25ms shell extraction around it.
bool IconBitmapFromHBitmap(HBITMAP bitmap, IconBitmap& out) {
    if (!bitmap) return false;
    BITMAP plain{};
    if (GetObjectW(bitmap, sizeof(plain), &plain) != sizeof(plain)) return false;
    if (plain.bmWidth <= 0 || plain.bmHeight <= 0) return false;

    const std::size_t stride = static_cast<std::size_t>(plain.bmWidth) * 4;
    out.width = plain.bmWidth;
    out.height = plain.bmHeight;
    out.pixels.assign(stride * static_cast<std::size_t>(plain.bmHeight), 0);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = plain.bmWidth;
    // Negative height requests top-down rows, matching every D2D bitmap we create.
    info.bmiHeader.biHeight = -plain.bmHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int rows = GetDIBits(dc, bitmap, 0, static_cast<UINT>(plain.bmHeight),
                               out.pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return rows == plain.bmHeight;
}

std::size_t IconCache::KeyHash::operator()(const Key& key) const {
    // FNV-1a over the UTF-16 code units plus the size, so two sources that differ
    // only in case still land in different buckets.
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t ch : key.source) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(key.size_px);
    hash *= 1099511628211ull;
    return static_cast<std::size_t>(hash);
}

std::wstring IconCache::CacheKey(const std::wstring& source, int size_px) {
    // Cache format version: the pre-fix extraction copied bmBits rows without checking
    // the DIB section's bottom-up row order, so every v1 file (bare name, no prefix)
    // stores a vertically flipped icon (evidence/T-200/icon_flip_spike.txt). The
    // version lives in the literal on purpose: a wide swprintf with %s reads the wide
    // argument as narrow and truncates at its first NUL byte, which silently produced
    // "v_" instead of "v2_" on the first attempt of this fix.
    constexpr wchar_t kKeyFormat[] = L"v2_%016llx_%d.png";
    std::uint64_t hash = 1469598103934665603ull;
    for (const wchar_t ch : source) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(size_px);
    hash *= 1099511628211ull;

    wchar_t buffer[48]{};
    std::swprintf(buffer, ARRAYSIZE(buffer), kKeyFormat,
                  static_cast<unsigned long long>(hash), size_px);
    return buffer;
}

bool IconCache::Initialize(const std::wstring& cache_dir) {
    if (cache_dir.empty()) return false;
    if (!core::EnsureDirectory(cache_dir)) {
        DF_WARN("shell", "cannot create icon cache dir %s",
                core::WideToUtf8(cache_dir).c_str());
        return false;
    }
    cache_dir_ = cache_dir;
    return true;
}

std::wstring IconCache::DiskPathFor(const std::wstring& source, int size_px) const {
    if (cache_dir_.empty()) return {};
    return cache_dir_ + L"\\" + CacheKey(source, size_px);
}

void IconCache::ClearMemory() { memory_.clear(); }

bool IconCache::ExtractDirect(const std::wstring& source, int size_px, IconBitmap& out) {
    if (source.empty() || size_px <= 0) return false;

    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(source.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr)) {
        // A missing path is an expected condition, not an error worth a warning:
        // the Dock hits it for every uninstalled Start-menu entry it remembers.
        DF_DEBUG("shell", "icon source %s unresolved (0x%08lX)",
                 core::WideToUtf8(source).c_str(), static_cast<unsigned long>(hr));
        return false;
    }

    ComPtr<IShellItemImageFactory> factory;
    hr = item.As(&factory);
    if (FAILED(hr)) return false;

    const std::int64_t start = core::QueryPerformanceTicks();
    HBITMAP bitmap = nullptr;
    hr = factory->GetImage(SIZE{size_px, size_px},
                           static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY),
                           &bitmap);
    const double elapsed_ms =
        core::QpcToMilliseconds(core::QueryPerformanceTicks() - start);

    if (FAILED(hr) || !bitmap) {
        DF_DEBUG("shell", "GetImage(%s) failed: 0x%08lX", core::WideToUtf8(source).c_str(),
                 static_cast<unsigned long>(hr));
        return false;
    }

    const bool ok = IconBitmapFromHBitmap(bitmap, out);
    DeleteObject(bitmap);

    ++stats_.extractions;
    stats_.last_extract_ms = elapsed_ms;
    stats_.total_extract_ms += elapsed_ms;
    return ok;
}

bool IconCache::SaveToDisk(const std::wstring& path, const IconBitmap& bitmap) {
    if (path.empty() || !bitmap.valid()) return false;

    ComPtr<IWICImagingFactory> wic = CreateWicFactory();
    if (!wic) return false;

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream))) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    if (FAILED(frame->SetSize(static_cast<UINT>(bitmap.width),
                              static_cast<UINT>(bitmap.height)))) {
        return false;
    }

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format))) return false;
    if (!IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) return false;

    // PNG wants straight alpha; the buffer holds premultiplied values.
    std::vector<std::uint8_t> straight = bitmap.pixels;
    Unpremultiply(straight);

    const UINT stride = static_cast<UINT>(bitmap.width) * 4;
    if (FAILED(frame->WritePixels(static_cast<UINT>(bitmap.height), stride,
                                  static_cast<UINT>(straight.size()), straight.data()))) {
        return false;
    }
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    return true;
}

bool IconCache::LoadFromDisk(const std::wstring& path, int expected_size, IconBitmap& out) {
    if (path.empty()) return false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    ComPtr<IWICImagingFactory> wic = CreateWicFactory();
    if (!wic) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnDemand, &decoder))) {
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;

    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height))) return false;
    if (width == 0 || height == 0) return false;
    // A cache file written for a different size means the key collided or the
    // file was tampered with; re-extract rather than draw a wrong-sized tile.
    if (static_cast<int>(width) != expected_size ||
        static_cast<int>(height) != expected_size) {
        return false;
    }

    // The converter both normalises the pixel format and premultiplies the alpha,
    // which is the representation the renderer consumes.
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
        return false;
    }

    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    const UINT stride = width * 4;
    out.pixels.assign(static_cast<std::size_t>(stride) * height, 0);
    if (FAILED(converter->CopyPixels(nullptr, stride, static_cast<UINT>(out.pixels.size()),
                                     out.pixels.data()))) {
        out = IconBitmap{};
        return false;
    }
    return true;
}

bool IconCache::Get(const std::wstring& source, int size_px, IconBitmap& out) {
    if (source.empty() || size_px <= 0) return false;

    const Key key{source, size_px};
    if (const auto it = memory_.find(key); it != memory_.end()) {
        ++stats_.memory_hits;
        out = it->second;
        return true;
    }

    const std::wstring disk_path = DiskPathFor(source, size_px);
    if (!disk_path.empty() && LoadFromDisk(disk_path, size_px, out)) {
        ++stats_.disk_hits;
        memory_.emplace(key, out);
        return true;
    }

    if (!ExtractDirect(source, size_px, out)) {
        ++stats_.failures;
        return false;
    }

    // Persisting is best-effort: a read-only or full cache directory must not stop
    // the Dock from drawing.
    if (!disk_path.empty()) SaveToDisk(disk_path, out);
    memory_.emplace(key, out);
    return true;
}

}  // namespace dockforge::shell
