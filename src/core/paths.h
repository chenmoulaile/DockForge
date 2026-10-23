// DockForge - known-folder path resolution (CONTRACTS.md C-03).
#pragma once

#include <string>

namespace dockforge::core {

// %APPDATA%\DockForge  (roaming)
std::wstring ConfigDir();

// %APPDATA%\DockForge\config.json
//
// Overridable with DOCKFORGE_CONFIG_PATH so the test binary and the evidence probes
// cannot write into the real profile. Without it, running dockforge_tests.exe on a
// developer machine rewrites the live dock arrangement - measured: a test case that
// exercises the order serializer left `tiles: ["alpha.app"]` in the user's config and
// the next launch came up with a single tile.
std::wstring ConfigPath();

// %APPDATA%\DockForge\themes
std::wstring ThemesDir();

// %LOCALAPPDATA%\DockForge\logs
std::wstring LogsDir();

// %LOCALAPPDATA%\DockForge\dumps
std::wstring DumpsDir();

// %LOCALAPPDATA%\DockForge\icons  (extracted icon PNG cache, CONTRACTS.md C-08)
std::wstring IconCacheDir();

// Creates a directory and its parents. Returns false on failure (already
// existing directory counts as success).
bool EnsureDirectory(const std::wstring& path);

// Executable directory (no trailing backslash).
std::wstring ExecutableDir();

// Converts UTF-8 <-> UTF-16. Invalid input yields an empty string.
std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

}  // namespace dockforge::core
