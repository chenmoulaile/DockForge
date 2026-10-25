// Tests for src/shell/icon_cache.* (CONTRACTS.md C-08).
//
// Extraction itself needs a real shell item, so these tests use notepad.exe - a file
// that exists on every supported Windows build - and treat everything else as a
// failure path. Disk-cache behaviour is tested through the public API so the on-disk
// layout stays an implementation detail.
#include "catch_amalgamated.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

#include "core/paths.h"
#include "shell/icon_cache.h"

using namespace dockforge::shell;

namespace {

std::wstring NotepadPath() {
    wchar_t windir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"windir", windir, MAX_PATH) == 0) return {};
    return std::wstring(windir) + L"\\system32\\notepad.exe";
}

// Removes every directory TempCacheDir handed out, at process exit.
//
// The cache is persistent by design, so the tests cannot share one directory - but
// leaving them behind is not free either: this machine had 648 of them, and each
// leftover is a future name collision for the very assertion above.
struct CacheDirReaper {
    ~CacheDirReaper() {
        for (const std::wstring& dir : *dirs) {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((dir + L"\\*").c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    if (data.cFileName[0] == L'.') continue;
                    DeleteFileW((dir + L"\\" + data.cFileName).c_str());
                } while (FindNextFileW(find, &data));
                FindClose(find);
            }
            RemoveDirectoryW(dir.c_str());
        }
        dirs->clear();
    }
    std::vector<std::wstring>* dirs;
};

std::vector<std::wstring>& CreatedDirs() {
    static std::vector<std::wstring> dirs;
    static CacheDirReaper reaper{&dirs};
    (void)reaper;
    return dirs;
}

std::wstring TempCacheDir() {
    // A unique directory per call: the disk cache is persistent by design, so a
    // shared path would let a previous run turn "must extract" into a disk hit and
    // silently invert the assertions that check extraction counts.
    //
    // PID plus a counter is NOT unique across time: Windows reuses PIDs, and a
    // leftover directory from an earlier run restarts the counter at 1 in the same
    // place. Measured: 1 failure in 25 full-suite runs, always with extractions==0
    // right after a successful Get. The tick count closes that hole, and the reaper
    // stops the leftovers accumulating in the first place.
    static int counter = 0;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    wchar_t name[96]{};
    std::swprintf(name, ARRAYSIZE(name), L"dockforge-test-icons-%lu-%llu-%d",
                  static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(GetTickCount64()), ++counter);
    std::wstring dir = std::wstring(temp) + name;
    CreatedDirs().push_back(dir);
    return dir;
}

}  // namespace

TEST_CASE("CacheKey is stable, size-sensitive and ends in .png") {
    const std::wstring a = IconCache::CacheKey(L"C:\\app.exe", 256);
    const std::wstring b = IconCache::CacheKey(L"C:\\app.exe", 256);
    CHECK(a == b);
    CHECK(a.find(L".png") != std::wstring::npos);

    CHECK(IconCache::CacheKey(L"C:\\app.exe", 128) != a);
    CHECK(IconCache::CacheKey(L"C:\\other.exe", 256) != a);
    // Case matters: Windows paths are case-insensitive but parsing names are not,
    // and a collision here would show the wrong icon.
    CHECK(IconCache::CacheKey(L"C:\\APP.EXE", 256) != a);
}

TEST_CASE("DiskPathFor is empty before Initialize") {
    IconCache cache;
    CHECK(cache.DiskPathFor(L"C:\\app.exe", 256).empty());
}

TEST_CASE("Initialize creates the cache directory") {
    IconCache cache;
    const std::wstring dir = TempCacheDir();
    REQUIRE(cache.Initialize(dir));
    CHECK(GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES);
    CHECK(!cache.DiskPathFor(L"C:\\app.exe", 256).empty());
    CHECK(cache.DiskPathFor(L"C:\\app.exe", 256).rfind(dir, 0) == 0);
}

TEST_CASE("Get rejects empty source and non-positive size without touching stats") {
    IconCache cache;
    cache.Initialize(TempCacheDir());
    IconBitmap bitmap;
    CHECK_FALSE(cache.Get(L"", 256, bitmap));
    CHECK_FALSE(cache.Get(L"C:\\app.exe", 0, bitmap));
    CHECK(cache.Stats().failures == 0);
}

TEST_CASE("Get fails cleanly for a source the shell cannot resolve") {
    IconCache cache;
    cache.Initialize(TempCacheDir());
    IconBitmap bitmap;
    CHECK_FALSE(cache.Get(L"C:\\DockForge\\definitely-not-here.exe", 64, bitmap));
    CHECK_FALSE(bitmap.valid());
    CHECK(cache.Stats().failures == 1);
    CHECK(cache.MemoryEntryCount() == 0);
}

TEST_CASE("Get extracts a real icon and serves the second call from memory") {
    const std::wstring notepad = NotepadPath();
    REQUIRE(!notepad.empty());
    REQUIRE(GetFileAttributesW(notepad.c_str()) != INVALID_FILE_ATTRIBUTES);

    IconCache cache;
    cache.Initialize(TempCacheDir());
    cache.ClearMemory();

    IconBitmap first;
    REQUIRE(cache.Get(notepad, 64, first));
    CHECK(first.valid());
    CHECK(first.width == 64);
    CHECK(first.height == 64);
    CHECK(first.pixels.size() == static_cast<std::size_t>(64 * 64 * 4));
    CHECK(cache.Stats().extractions >= 1);
    CHECK(cache.Stats().last_extract_ms > 0.0);

    const std::uint64_t extractions_before = cache.Stats().extractions;
    IconBitmap second;
    REQUIRE(cache.Get(notepad, 64, second));
    CHECK(cache.Stats().memory_hits == 1);
    // A memory hit must not re-run the shell call.
    CHECK(cache.Stats().extractions == extractions_before);
    CHECK(second.pixels == first.pixels);
}

TEST_CASE("Disk cache round-trips pixels for a fresh cache instance") {
    const std::wstring notepad = NotepadPath();
    REQUIRE(!notepad.empty());
    const std::wstring dir = TempCacheDir();

    IconBitmap original;
    {
        IconCache warm;
        warm.Initialize(dir);
        warm.ClearMemory();
        REQUIRE(warm.Get(notepad, 48, original));
        REQUIRE(warm.Stats().extractions == 1);
    }

    // A new instance shares no memory, so a hit here can only come from disk.
    IconCache cold;
    cold.Initialize(dir);
    IconBitmap cached;
    REQUIRE(cold.Get(notepad, 48, cached));
    CHECK(cold.Stats().disk_hits == 1);
    CHECK(cold.Stats().extractions == 0);
    CHECK(cached.width == 48);
    CHECK(cached.height == 48);
    CHECK(cached.pixels.size() == original.pixels.size());
}

TEST_CASE("ClearMemory forces a disk or extraction path again") {
    const std::wstring notepad = NotepadPath();
    REQUIRE(!notepad.empty());

    IconCache cache;
    cache.Initialize(TempCacheDir());
    IconBitmap bitmap;
    REQUIRE(cache.Get(notepad, 32, bitmap));
    REQUIRE(cache.MemoryEntryCount() == 1);
    cache.ClearMemory();
    CHECK(cache.MemoryEntryCount() == 0);
    REQUIRE(cache.Get(notepad, 32, bitmap));
    CHECK(cache.Stats().disk_hits + cache.Stats().memory_hits >= 1);
}

// ------------------------------------------------------- row order (icon flip)

TEST_CASE("A bottom-up DIB section comes out top-down", "[shell][icons][flip]") {
    // IShellItemImageFactory::GetImage hands back a BOTTOM-UP DIB section (measured,
    // spikes/icon_flip_probe.cpp). Reading bmBits 0..h-1 as if it were top-down drew
    // every shell icon upside down and baked the flip into the disk cache. This case
    // pins the normalisation: the visually top row of the section must land at output
    // row 0.
    const int width = 8;
    const int height = 4;
    BITMAPV5HEADER info{};
    info.bV5Size = sizeof(info);
    info.bV5Width = width;
    // Positive height: the section's memory row 0 is the visually BOTTOM row.
    info.bV5Height = height;
    info.bV5Planes = 1;
    info.bV5BitCount = 32;
    info.bV5Compression = BI_BITFIELDS;
    info.bV5RedMask = 0x00FF0000;
    info.bV5GreenMask = 0x0000FF00;
    info.bV5BlueMask = 0x000000FF;
    info.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    REQUIRE(dc != nullptr);
    HBITMAP section = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&info),
                                       DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    REQUIRE(section != nullptr);
    REQUIRE(bits != nullptr);

    // Paint the visually-bottom row (memory row 0) opaque red, everything else clear.
    auto* pixels = static_cast<std::uint32_t*>(bits);
    for (int x = 0; x < width; ++x) pixels[x] = 0xFFFF0000;  // BGRA(0,0,255,255): opaque red, little-endian

    IconBitmap out;
    REQUIRE(IconBitmapFromHBitmap(section, out));
    REQUIRE(out.width == width);
    REQUIRE(out.height == height);
    REQUIRE(out.pixels.size() == static_cast<std::size_t>(width) * height * 4);

    // Output row 0 is the visually TOP row: transparent here.
    for (int x = 0; x < width; ++x) {
        const std::uint8_t* px = out.pixels.data() + x * 4;
        INFO("top row, x=" << x);
        CHECK(px[3] == 0);
    }
    // Output row height-1 is the visually BOTTOM row: opaque red.
    for (int x = 0; x < width; ++x) {
        const std::uint8_t* px =
            out.pixels.data() + (static_cast<std::size_t>(height - 1) * width + x) * 4;
        INFO("bottom row, x=" << x);
        CHECK(px[3] == 0xFF);
        CHECK(px[2] == 0xFF);  // red channel in BGRA
    }
    DeleteObject(section);
}

TEST_CASE("A top-down DIB section is not flipped twice", "[shell][icons][flip]") {
    const int width = 8;
    const int height = 4;
    BITMAPV5HEADER info{};
    info.bV5Size = sizeof(info);
    info.bV5Width = width;
    info.bV5Height = -height;  // negative: memory row 0 is the visually TOP row
    info.bV5Planes = 1;
    info.bV5BitCount = 32;
    info.bV5Compression = BI_BITFIELDS;
    info.bV5RedMask = 0x00FF0000;
    info.bV5GreenMask = 0x0000FF00;
    info.bV5BlueMask = 0x000000FF;
    info.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    REQUIRE(dc != nullptr);
    HBITMAP section = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&info),
                                       DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    REQUIRE(section != nullptr);
    REQUIRE(bits != nullptr);

    auto* pixels = static_cast<std::uint32_t*>(bits);
    for (int x = 0; x < width; ++x) pixels[x] = 0xFFFF0000;

    IconBitmap out;
    REQUIRE(IconBitmapFromHBitmap(section, out));
    REQUIRE(out.height == height);
    for (int x = 0; x < width; ++x) {
        const std::uint8_t* px = out.pixels.data() + x * 4;
        CHECK(px[3] == 0xFF);
        CHECK(px[2] == 0xFF);
    }
    const std::uint8_t* last = out.pixels.data() +
                               (static_cast<std::size_t>(height - 1) * width) * 4;
    CHECK(last[3] == 0);
    DeleteObject(section);
}
