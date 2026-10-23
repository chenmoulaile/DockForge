// DockForge - icon extraction with memory and disk caching.
// Contract: CONTRACTS.md C-08.
//
// Cold extraction costs 60-190 ms per icon (measured, see evidence/T-006), while a
// warm shell cache hit costs ~2 ms. The Dock resolves its whole tile list before the
// first frame, so the disk cache is what keeps cold start under the 800 ms budget on
// the second run and beyond.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dockforge::shell {

// Pixels as the renderer wants them: premultiplied BGRA, top-down, no padding.
struct IconBitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
    std::size_t ByteSize() const { return pixels.size(); }
};

struct IconCacheStats {
    std::uint64_t memory_hits = 0;
    std::uint64_t disk_hits = 0;
    std::uint64_t extractions = 0;
    std::uint64_t failures = 0;
    double last_extract_ms = 0.0;
    double total_extract_ms = 0.0;

    double AverageExtractMs() const {
        return extractions ? total_extract_ms / static_cast<double>(extractions) : 0.0;
    }
};

class IconCache {
public:
    // Creates the on-disk cache directory. Safe to call more than once.
    bool Initialize(const std::wstring& cache_dir);

    // Loads a 32-bit icon of `size_px` for a shell item. `source` is either a
    // filesystem path or a parsing name such as "shell:AppsFolder\<AUMID>".
    // Returns false and leaves `out` untouched when the shell cannot produce one.
    bool Get(const std::wstring& source, int size_px, IconBitmap& out);

    // Same as Get() but never consults the disk cache; used by the cache warm-up
    // path and by tests that must observe real extraction cost.
    bool ExtractDirect(const std::wstring& source, int size_px, IconBitmap& out);

    const IconCacheStats& Stats() const { return stats_; }
    std::size_t MemoryEntryCount() const { return memory_.size(); }
    void ClearMemory();

    // Absolute path the disk cache uses for this item (empty before Initialize()).
    std::wstring DiskPathFor(const std::wstring& source, int size_px) const;

    // Stable, filesystem-safe key derived from the source string. Exposed so the
    // cache layout is testable without touching the shell.
    static std::wstring CacheKey(const std::wstring& source, int size_px);

private:
    struct Key {
        std::wstring source;
        int size_px = 0;
        bool operator==(const Key& other) const {
            return size_px == other.size_px && source == other.source;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const;
    };

    bool LoadFromDisk(const std::wstring& path, int expected_size, IconBitmap& out);
    bool SaveToDisk(const std::wstring& path, const IconBitmap& bitmap);

    std::wstring cache_dir_;
    std::unordered_map<Key, IconBitmap, KeyHash> memory_;
    IconCacheStats stats_;
};

}  // namespace dockforge::shell
