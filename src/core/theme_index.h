// DockForge - community theme index (T-085, ADR-0026).
//
// The distribution format is a STATIC JSON index served from the project's own
// hosting (default: raw.githubusercontent.com/chenmoulaile/DockForge/main/
// theme-index/index.json, CONTRACTS C-03 themes/ and core::DefaultThemeIndexUrl).
// No Steam Workshop, no ratings, no comments, no subscription feed (ADR-0026
// 禁止条款) - the index is a plain file, the browser renders it, that is all.
//
// Schema (schema 1):
//   {
//     "schema": 1,
//     "updated": "2026-11-20",
//     "themes": [ {
//       "name": "mono", "author": "dockforge", "version": "1.0.0",
//       "preview_url": "https://...",          // optional in v1: the browser
//                                               // lists it, no image decode
//       "download_url": "https://.../mono-1.0.0.dockforge-theme",
//       "sha256": "<64 hex, case-insensitive>", // REQUIRED: install refuses
//                                                // on any mismatch
//       "size_bytes": 1204                      // optional sanity bound
//     }, ... ]
//   }
//
// Parsing is pure and unit-tested; the network side (FetchThemeIndex) is a
// thin platform::HttpGetBytes wrapper so a probe can point the URL anywhere.
#pragma once

#include <string>
#include <vector>

namespace dockforge::core {

// One row of the remote index. `sha256` is normalized to lowercase hex at
// parse time so the installer compares a single canonical form.
struct ThemeIndexEntry {
    std::string name;
    std::string author;
    std::string version;
    std::string preview_url;
    std::string download_url;
    std::string sha256;
    long long size_bytes = 0;  // 0 = not advertised
};

// True when `name` may be used as a theme directory: [a-z0-9._-] lowercase
// only, 1..64 bytes, never "default"/"dark" (built-ins), never "..". The index
// parser applies this per entry; the installer applies it again (defense in
// depth, tests lock both).
bool IsValidThemeName(const std::string& name);

// Parses an index document. Returns false on malformed JSON, missing "themes"
// array, unknown "schema", or any per-entry validation failure - a poisoned
// index must never reach the browser half-parsed. On success `out` holds the
// entries in file order.
bool ParseThemeIndex(const std::string& json_text, std::vector<ThemeIndexEntry>& out,
                     std::string* error = nullptr);

// Fetches and parses the index at `url_utf8` (blocking; worker thread only).
// `error` carries a human-readable reason on failure.
bool FetchThemeIndex(const std::string& url_utf8, std::vector<ThemeIndexEntry>& out,
                     std::string* error = nullptr);

}  // namespace dockforge::core
