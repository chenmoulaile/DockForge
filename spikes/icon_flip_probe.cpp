// Spike: does BitmapToPixels' bmBits fast path return a vertically flipped image?
// Extracts one icon through shell::IconCache::ExtractDirect (production path) and
// through a forced GetDIBits(-height) reference, then prints both as ASCII so the
// orientation is visible, plus a row-luma checksum comparison.
#include <windows.h>
#include <shobjidl.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct RefBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;  // top-down BGRA
};

// Reference: always asks GetDIBits for top-down rows; correct regardless of the
// source DIB's row order.
RefBitmap Reference(HBITMAP bitmap) {
    BITMAP bm{};
    if (GetObjectW(bitmap, sizeof(bm), &bm) != sizeof(bm)) return {};
    RefBitmap out;
    out.width = bm.bmWidth;
    out.height = bm.bmHeight;
    out.pixels.assign(static_cast<std::size_t>(bm.bmWidth) * 4 * bm.bmHeight, 0);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = bm.bmWidth;
    info.bmiHeader.biHeight = -bm.bmHeight;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, bitmap, 0, bm.bmHeight, out.pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return out;
}

void PrintAscii(const char* label, int width, int height,
                const std::vector<std::uint8_t>& pixels) {
    std::printf("== %s (%dx%d)\n", label, width, height);
    for (int gy = 0; gy < 10; ++gy) {
        std::string row;
        for (int gx = 0; gx < 24; ++gx) {
            const int x = gx * width / 24;
            const int y = gy * height / 10;
            const std::uint8_t* px = pixels.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            const double luma = 0.299 * px[2] + 0.587 * px[1] + 0.114 * px[0];
            if (px[3] < 32) row += ' ';
            else if (luma > 160) row += '#';
            else if (luma > 90) row += '+';
            else if (luma > 40) row += '.';
            else row += '@';
        }
        std::printf("%s\n", row.c_str());
    }
}

double RowLuma(int row, int width, const std::vector<std::uint8_t>& pixels) {
    double sum = 0.0;
    for (int x = 0; x < width; ++x) {
        const std::uint8_t* px = pixels.data() + (static_cast<std::size_t>(row) * width + x) * 4;
        sum += 0.299 * px[2] + 0.587 * px[1] + 0.114 * px[0];
    }
    return sum / width;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* source = argc > 1 ? argv[1] : L"C:\\Windows\\System32\\cmd.exe";
    const int size_px = argc > 2 ? _wtoi(argv[2]) : 192;

    HBITMAP bitmap = nullptr;
    {
        // Same call the production ExtractDirect makes, kept independent of the
        // production include graph on purpose.
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IShellItem* item = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(source, nullptr, IID_PPV_ARGS(&item)))) {
            IShellItemImageFactory* factory = nullptr;
            if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&factory)))) {
                factory->GetImage(SIZE{size_px, size_px},
                                  static_cast<SIIGBF>(SIIGBF_BIGGERSIZEOK | SIIGBF_ICONONLY),
                                  &bitmap);
                factory->Release();
            }
            item->Release();
        }
    }
    if (!bitmap) {
        std::printf("GetImage failed for %ls\n", source);
        return 1;
    }

    // Report what the HBITMAP actually is.
    DIBSECTION ds{};
    const int object_bytes = GetObjectW(bitmap, sizeof(ds), &ds);
    if (object_bytes == sizeof(DIBSECTION)) {
        std::printf("HBITMAP: DIB section, dsBmih.biHeight=%d (%s), bpp=%d\n",
                    ds.dsBmih.biHeight,
                    ds.dsBmih.biHeight > 0 ? "BOTTOM-UP" : "top-down",
                    ds.dsBmih.biBitCount);
    } else {
        BITMAP bm{};
        GetObjectW(bitmap, sizeof(bm), &bm);
        std::printf("HBITMAP: DDB (GetObject bytes=%d), bmBits=%p\n", object_bytes,
                    bm.bmBits);
    }

    // Production path: copy bmBits rows in file order (what the fast path does).
    BITMAP bm{};
    GetObjectW(bitmap, sizeof(bm), &bm);
    const std::size_t stride = static_cast<std::size_t>(bm.bmWidth) * 4;
    std::vector<std::uint8_t> fast(stride * bm.bmHeight, 0);
    if (bm.bmBits) {
        const auto* src = static_cast<const std::uint8_t*>(bm.bmBits);
        const std::size_t src_stride =
            bm.bmWidthBytes > 0 ? static_cast<std::size_t>(bm.bmWidthBytes) : stride;
        for (int y = 0; y < bm.bmHeight; ++y) {
            memcpy(fast.data() + y * stride, src + y * src_stride, stride);
        }
        PrintAscii("fast path (bmBits, row order as stored)", bm.bmWidth, bm.bmHeight, fast);
    } else {
        std::printf("bmBits is null; the fast path would NOT trigger for this icon\n");
    }

    const RefBitmap ref = Reference(bitmap);
    PrintAscii("reference (GetDIBits top-down)", ref.width, ref.height, ref.pixels);

    if (bm.bmBits && !ref.pixels.empty()) {
        const double fast_first = RowLuma(0, bm.bmWidth, fast);
        const double ref_first = RowLuma(0, ref.width, ref.pixels);
        const double ref_last = RowLuma(ref.height - 1, ref.width, ref.pixels);
        std::printf("row0 luma: fast=%.1f ref=%.1f ref_last=%.1f -> fast matches %s\n",
                    fast_first, ref_first, ref_last,
                    fast_first == ref_first ? "TOP (correct order)"
                                            : (fast_first == ref_last ? "BOTTOM (FLIPPED)"
                                                                      : "?"));
    }
    DeleteObject(bitmap);
    return 0;
}
