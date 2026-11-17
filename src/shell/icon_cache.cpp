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

// Alpha layout, measured rather than assumed.
//
// spikes/T-301_finderbar_menu/alpha_check.cpp dumps IShellItemImageFactory output and
// finds texels like B=127 G=255 R=255 A=2 - a colour channel far above alpha, which is
// arithmetically impossible for premultiplied data. 6215 of 9427 partial-alpha texels
// violate rgb<=a for notepad.exe, 78 of 202 for explorer.exe, and a synthetic
// straight-alpha DIB of known content (R=255,A=64) survives the same GetDIBits copy
// unchanged. So the shell hands us STRAIGHT alpha while backend_d2d tags the texture
// D2D1_ALPHA_MODE_PREMULTIPLIED, and every anti-aliased edge is drawn brighter than it
// should be - the halo that made dock icons look worse than the native Windows icon.
//
// The conversion is conditional on proof, not on faith: a texel with any colour channel
// above its alpha can only come from straight-alpha data, so we premultiply exactly when
// such a texel exists. Buffers that are already premultiplied (or fully opaque) are left
// untouched, because multiplying them again would darken them - the mirror image of the
// bug being fixed. Returns true when the buffer was converted.
bool PremultiplyIfStraight(std::vector<std::uint8_t>& bgra) {
    bool straight = false;
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const unsigned a = bgra[i + 3];
        if (a == 0 || a == 255) continue;
        if (bgra[i] > a || bgra[i + 1] > a || bgra[i + 2] > a) {
            straight = true;
            break;
        }
    }
    if (!straight) return false;
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const unsigned a = bgra[i + 3];
        if (a == 0) {
            bgra[i] = 0;
            bgra[i + 1] = 0;
            bgra[i + 2] = 0;
            continue;
        }
        if (a == 255) continue;
        for (int c = 0; c < 3; ++c) {
            bgra[i + c] = static_cast<std::uint8_t>((bgra[i + c] * a + 127u) / 255u);
        }
    }
    return true;
}

// Inverse of the above, used only on the way to disk: PNG stores straight alpha, so a
// premultiplied buffer has to be divided back out before encoding. Kept next to
// PremultiplyIfStraight on purpose - the two must stay exact inverses, and the original
// bug was a comment claiming this division had already happened on extraction.
void Unpremultiply(std::vector<std::uint8_t>& bgra) {
    for (std::size_t i = 0; i + 3 < bgra.size(); i += 4) {
        const unsigned a = bgra[i + 3];
        if (a == 0) {
            bgra[i] = 0;
            bgra[i + 1] = 0;
            bgra[i + 2] = 0;
            continue;
        }
        if (a == 255) continue;
        for (int c = 0; c < 3; ++c) {
            const unsigned scaled = (bgra[i + c] * 255u + a / 2u) / a;
            bgra[i + c] = static_cast<std::uint8_t>(scaled > 255u ? 255u : scaled);
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
// Declared before its only caller: the trim is part of every extraction path
// (see the block above IconBitmapFromHBitmap's neighbours for the rationale).
void TrimShellDecoration(IconBitmap& bitmap);

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
    if (rows != plain.bmHeight) return false;

    // IconBitmap is documented as premultiplied because that is what the D2D texture
    // tag demands; the shell gives straight alpha. See PremultiplyIfStraight.
    PremultiplyIfStraight(out.pixels);
    TrimShellDecoration(out);
    return true;
}

// ---- shell decoration trim (ADR-0058) ------------------------------------------
//
// The fidelity acceptance measured the dock's "8 icons in outlined boxes, glyph
// only ~50% inside": the shell's IShellItemImageFactory, asked for a 256px ICONONLY
// bitmap of a source whose largest icon frame is small (32px AHK/CHM/7z-era
// resources), returns a 256px COMPOSITION - the icon content centred at its native
// size, surrounded by transparency and a thin decorative outline rectangle hugging
// the canvas edge (evidence/fidelity-fix/cache_12fad1fed92dd8c7_256.png). Drawing
// that composition 1:1 is what produced the "box with a tiny glyph inside" look;
// the macOS reference dock draws every icon full-bleed.
//
// The trim runs on every extracted bitmap before it reaches the cache. Detection is
// measured, not assumed: an outline composition has an opaque edge ring and a
// TRANSPARENT band just inside it; a real full-bleed icon (7-Zip's white card, the
// Bandizip diamond) is opaque or partially covered right up to its own edge. So:
//
//   outer ring opacity high  &&  inner band opacity low  ->  outline composition,
//   crop everything outside the inner band's content bounding box;
//   anything else stays exactly as the shell returned it.
namespace {

struct BBox {
    int left = 0;
    int top = 0;
    int right = 0;   // exclusive
    int bottom = 0;  // exclusive
    bool empty = true;
};

BBox AlphaBoundingBox(const IconBitmap& b, int x0, int y0, int x1, int y1,
                      std::uint8_t alpha_min) {
    BBox box;
    box.left = x1;
    box.top = y1;
    x1 = std::min(x1, b.width);
    y1 = std::min(y1, b.height);
    for (int y = std::max(0, y0); y < y1; ++y) {
        const std::uint8_t* row = b.pixels.data() + static_cast<std::size_t>(y) * b.width * 4;
        for (int x = std::max(0, x0); x < x1; ++x) {
            if (row[static_cast<std::size_t>(x) * 4 + 3] >= alpha_min) {
                box.empty = false;
                box.left = std::min(box.left, x);
                box.right = std::max(box.right, x + 1);
                box.top = std::min(box.top, y);
                box.bottom = std::max(box.bottom, y + 1);
            }
        }
    }
    return box;
}

}  // namespace

void TrimShellDecoration(IconBitmap& bitmap) {
    if (!bitmap.valid() || bitmap.width < 24 || bitmap.height < 24) return;
    const BBox content = AlphaBoundingBox(bitmap, 0, 0, bitmap.width, bitmap.height, 10);
    if (content.empty) return;
    // First drop any transparent margin around the whole composition (the shell
    // does not always draw its outline at the canvas edge; a Win11-design icon's
    // own padding tightens the same way, which is exactly the full-bleed look the
    // reference dock draws).
    if (content.left > 0 || content.top > 0 || content.right < bitmap.width ||
        content.bottom < bitmap.height) {
        IconBitmap tightened;
        tightened.width = content.right - content.left;
        tightened.height = content.bottom - content.top;
        tightened.pixels.reserve(static_cast<std::size_t>(tightened.width) *
                                 tightened.height * 4);
        for (int y = content.top; y < content.bottom; ++y) {
            const std::uint8_t* src = bitmap.pixels.data() +
                                      (static_cast<std::size_t>(y) * bitmap.width +
                                       content.left) * 4;
            tightened.pixels.insert(tightened.pixels.end(), src,
                                    src + static_cast<std::size_t>(tightened.width) * 4);
        }
        bitmap = std::move(tightened);
    }
    // The outline composition (measured, evidence/fidelity-fix/ring_probe.ps1
    // against real cache files): a decorative rectangle hugging the artwork's
    // bounding box, transparency between it and the centred artwork. The DISCRIMINATOR
    // is area, not ring shape: inside the outline, the artwork's bounding box
    // collapses to a fraction of the canvas (the AHK H measured 38%, the CHM
    // composition ~14%), while every genuine icon keeps most of its canvas past a
    // 6px inset - the white 7-Zip card is opaque, the Bandizip diamond keeps
    // ~94%. So: bound the artwork excluding a 6px frame band; when that box
    // covers less than half the canvas, the frame band was decoration - crop it.
    const BBox art = AlphaBoundingBox(bitmap, 6, 6, bitmap.width - 6, bitmap.height - 6, 10);
    if (art.empty || art.right - art.left < 16 || art.bottom - art.top < 16) return;
    const long long art_area =
        static_cast<long long>(art.right - art.left) * (art.bottom - art.top);
    const long long canvas_area =
        static_cast<long long>(bitmap.width) * bitmap.height;
    if (art_area * 2 >= canvas_area) return;
    IconBitmap trimmed;
    trimmed.width = art.right - art.left;
    trimmed.height = art.bottom - art.top;
    trimmed.pixels.reserve(static_cast<std::size_t>(trimmed.width) * trimmed.height * 4);
    for (int y = art.top; y < art.bottom; ++y) {
        const std::uint8_t* src =
            bitmap.pixels.data() + (static_cast<std::size_t>(y) * bitmap.width + art.left) * 4;
        trimmed.pixels.insert(trimmed.pixels.end(), src,
                              src + static_cast<std::size_t>(trimmed.width) * 4);
    }
    bitmap = std::move(trimmed);
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
    // "v_" instead of "v2_" on the first attempt of that fix.
    //
    // v3 is the alpha-layout fix: v2 files hold straight-alpha pixels that were written
    // after an Unpremultiply pass, so they are doubly wrong and must not be reused.
    // v6 is the decoration-trim fix (ADR-0058): pre-v6 files may hold the shell's
    // outline-composition bitmaps (decorative frame + centred small artwork); the
    // area-based trim runs before save, so every pre-v6 file is stale by definition.
    // Bumping the version is the only safe way to invalidate them - nothing in the PNG
    // records which layout it was encoded from.
    constexpr wchar_t kKeyFormat[] = L"v6_%016llx_%d.png";
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

    // PNG wants straight alpha; IconBitmap holds premultiplied values (see
    // PremultiplyIfStraight), so the division is real work here rather than the
    // no-op the old comment implied.
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
    // A cache file must not EXCEED the requested size (the shell never returns
    // more than asked). It may legitimately be SMALLER: the ADR-0058 decoration
    // trim crops the shell's outline compositions to the artwork's bounding box
    // before save, so the stored dimensions are the artwork's, not the request's.
    // Larger means the key collided or the file was tampered with; re-extract
    // rather than draw a wrong-sized tile.
    if (static_cast<int>(width) > expected_size ||
        static_cast<int>(height) > expected_size) {
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
