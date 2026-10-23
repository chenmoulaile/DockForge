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

std::wstring TempCacheDir() {
    // A unique directory per call: the disk cache is persistent by design, so a
    // shared path would let a previous run turn "must extract" into a disk hit and
    // silently invert the assertions that check extraction counts.
    static int counter = 0;
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    wchar_t name[64]{};
    std::swprintf(name, ARRAYSIZE(name), L"dockforge-test-icons-%lu-%d",
                  static_cast<unsigned long>(GetCurrentProcessId()), ++counter);
    return std::wstring(temp) + name;
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
