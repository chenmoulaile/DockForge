// DockForge - community theme pack installation (T-085, ADR-0026, C-02).
//
// A `.dockforge-theme` file is a ZIP archive containing a C-02 theme pack:
//   theme.json (required) + preview.png (optional) + icons/ (optional)
// and never executable content. The installer:
//   1. compares the downloaded bytes' SHA256 (bcrypt) against the index entry;
//      any mismatch refuses the install (DoD: 校验 SHA256，失败拒绝安装);
//   2. unpacks with miniz into ThemesDir()\<name>.staging-<tick>;
//   3. validates the staging tree: theme.json must parse, every entry must
//      carry a non-executable extension (C-02 不得含可执行内容 - the
//      allow-list is json/png/jpg/jpeg/bmp/gif/webp/ttf/otf/woff/woff2);
//   4. swaps the staging tree into ThemesDir()\<name> (an existing community
//      pack of the same name is replaced; the two built-ins are refused).
//
// Every failure path cleans up its staging tree and leaves the installed set
// untouched. All functions are blocking - they run on a worker thread owned
// by the settings window, never the UI thread.
#pragma once

#include <string>

namespace dockforge::core {

// SHA256 of `bytes`, lowercase hex (64 chars). Returns false when bcrypt
// fails, which in practice means a broken system install - callers surface
// the error rather than treating it as "hash mismatch".
bool Sha256Hex(const std::string& bytes, std::string& hex_out, std::string& error);

// Unpacks ZIP bytes into `dest_dir` with miniz. Entry names are normalized
// ('\\' -> '/', the PowerShell 5.1 Compress-Archive separator measured in
// spikes/t085_theme_dist), every ".." component, drive prefix and absolute
// path rejects the whole archive, and parent directories are created as
// needed. Returns false with `error` filled; partial trees are possible on
// failure, so callers stage into a throwaway directory.
bool ExtractThemePackBytes(const std::string& bytes, const std::wstring& dest_dir,
                           std::string& error);

// C-02 conformance of an unpacked tree: theme.json parses, and no entry in
// the tree carries an executable or unknown extension. `error` names the
// first offending entry.
bool ValidateThemePackDir(const std::wstring& dir, std::string& error);

// Whole install. `expected_sha256_hex` is compared case-insensitively against
// the SHA256 of `pack_bytes`; on any mismatch nothing is written and `error`
// says "sha256 mismatch". On success the pack lives at ThemesDir()\<name> and
// the staging tree is gone.
struct ThemeInstallResult {
    bool ok = false;
    std::string error;
};
ThemeInstallResult InstallThemePackBytes(const std::string& pack_bytes,
                                         const std::string& expected_sha256_hex,
                                         const std::string& name);

// Deletes ThemesDir()\<name>. Built-ins ("default"/"dark") are refused;
// anything outside ThemesDir() is refused. Returns false with `error`.
bool UninstallTheme(const std::string& name, std::string& error);

}  // namespace dockforge::core
