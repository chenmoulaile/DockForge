// DockForge - community theme pack installation (T-085, ADR-0026, C-02).
#include "core/theme_install.h"

#include "core/log.h"
#include "core/paths.h"
#include "core/theme.h"
#include "core/theme_index.h"

#include <windows.h>

#include <bcrypt.h>

#include "miniz.h"

#include <cctype>
#include <cstdio>
#include <vector>

namespace dockforge::core {
namespace {

constexpr char kLogModule[] = "theme_install";

// C-02 "不得含可执行内容": the pack may carry documents and font/audio assets
// only. Anything else - the whole .exe/.dll/.bat/.ps1 family plus every
// extension this list does not know - rejects the pack. An allow-list (not a
// block-list) because the dangerous set is unbounded and the safe set is the
// contract.
bool IsAllowedPackExtension(const std::string& lower_ext) {
    static const char* kAllowed[] = {
        ".json",  // theme.json, calendar.json (T-207 对接点)
        ".png", ".jpg", ".jpeg", ".bmp", ".gif", ".webp",  // preview + icons
        ".ttf", ".otf", ".woff", ".woff2",                 // fonts (ASSETS.md)
    };
    for (const char* candidate : kAllowed) {
        if (lower_ext == candidate) return true;
    }
    return false;
}

std::wstring Utf8PathToWide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        needed);
    return out;
}

// Recursively deletes a directory tree; failure reported via bool (used on the
// staging path where a leftover must not keep the install from being refused).
bool RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = dir + L"\\*";
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTree(full);
            } else {
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return RemoveDirectoryW(dir.c_str()) != FALSE;
}

std::string LowerAscii(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool SameHexCaseInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

struct MinizWriteCtx {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool ok = true;
};

mz_uint64 MinizWriteCallback(void* opaque, mz_uint64 /*offset*/, const void* buffer,
                             size_t size) {
    auto* ctx = static_cast<MinizWriteCtx*>(opaque);
    if (!ctx->ok) return size;
    DWORD wrote = 0;
    if (!WriteFile(ctx->handle, buffer, static_cast<DWORD>(size), &wrote, nullptr) ||
        wrote != size) {
        ctx->ok = false;
    }
    return size;
}

}  // namespace

bool Sha256Hex(const std::string& bytes, std::string& hex_out, std::string& error) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, L"SHA256", nullptr, 0);
    if (st != 0) {
        error = "bcrypt open failed (" + std::to_string(st) + ")";
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (st == 0) {
        // Chunked: a large pack goes through BCryptHashData in 1MB slices.
        constexpr std::size_t kChunk = 1 << 20;
        if (bytes.empty()) {
            st = BCryptHashData(hash, nullptr, 0, 0);
        }
        for (std::size_t off = 0; st == 0 && off < bytes.size(); off += kChunk) {
            const std::size_t n = std::min(kChunk, bytes.size() - off);
            st = BCryptHashData(
                hash,
                reinterpret_cast<unsigned char*>(const_cast<char*>(bytes.data())) + off,
                static_cast<ULONG>(n), 0);
        }
    }
    unsigned char digest[32] = {};
    if (st == 0) {
        ULONG done = 0;
        st = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        if (st == 0) {
            static const char* digits = "0123456789abcdef";
            hex_out.clear();  // callers reuse the variable; this must replace
            hex_out.reserve(64);
            for (const unsigned char b : digest) {
                hex_out.push_back(digits[(b >> 4) & 0xF]);
                hex_out.push_back(digits[b & 0xF]);
            }
        }
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) {
        error = "bcrypt hash failed (" + std::to_string(st) + ")";
        return false;
    }
    return true;
}

bool ExtractThemePackBytes(const std::string& bytes, const std::wstring& dest_dir,
                           std::string& error) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) {
        error = "not a readable .dockforge-theme archive (err=" +
                std::to_string(mz_zip_get_last_error(&zip)) + ")";
        return false;
    }
    const unsigned count = mz_zip_reader_get_num_files(&zip);
    if (count == 0) {
        mz_zip_reader_end(&zip);
        error = "theme pack archive is empty";
        return false;
    }
    bool ok = true;
    for (unsigned i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            error = "archive entry " + std::to_string(i) + " unreadable";
            ok = false;
            break;
        }
        // Normalize the separator FIRST: PowerShell 5.1's Compress-Archive
        // writes 'icons\sample.png' (spikes/t085_theme_dist), which a naive
        // path join turns into an unresolved name.
        std::string name = stat.m_filename;
        for (char& c : name) {
            if (c == '\\') c = '/';
        }
        const bool traversal =
            name.find("..") != std::string::npos ||
            (!name.empty() && (name[0] == '/' || name[0] == '\\')) ||
            (name.size() >= 2 && name[1] == ':');
        if (traversal) {
            error = "pack entry escapes its root: '" + name + "'";
            ok = false;
            break;
        }
        const std::wstring rel = Utf8PathToWide(name);
        if (rel.empty()) {
            error = "pack entry has an unusable name";
            ok = false;
            break;
        }

        if (stat.m_is_directory) {
            const std::wstring dir = dest_dir + L"\\" + rel;
            if (!CreateDirectoryW(dir.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                error = "cannot create pack directory '" + name + "'";
                ok = false;
                break;
            }
            continue;
        }

        // Create parents (a pack does not have to list directories as
        // entries - many zip writers emit bare file paths). `rel` is
        // already normalized to forward slashes above, so search those.
        std::size_t start = 0;
        for (;;) {
            const std::size_t slash = rel.find(L'/', start);
            if (slash == std::wstring::npos) break;
            const std::wstring part = rel.substr(0, slash);
            if (!part.empty()) {
                const std::wstring dir = dest_dir + L"\\" + part;
                if (!CreateDirectoryW(dir.c_str(), nullptr) &&
                    GetLastError() != ERROR_ALREADY_EXISTS) {
                    error = "cannot create pack directory '" + name + "'";
                    ok = false;
                    break;
                }
            }
            start = slash + 1;
        }
        if (!ok) break;

        const std::wstring target = dest_dir + L"\\" + rel;
        HANDLE out = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) {
            error = "cannot write pack entry '" + name + "' (err=" +
                    std::to_string(GetLastError()) + ")";
            ok = false;
            break;
        }
        MinizWriteCtx ctx{};
        ctx.handle = out;
        if (!mz_zip_reader_extract_file_to_callback(&zip, stat.m_filename,
                                                    &MinizWriteCallback, &ctx, 0)) {
            ctx.ok = false;
        }
        CloseHandle(out);
        if (!ctx.ok) {
            error = "short write for pack entry '" + name + "'";
            ok = false;
            break;
        }
    }
    mz_zip_reader_end(&zip);
    return ok;
}

bool ValidateThemePackDir(const std::wstring& dir, std::string& error) {
    // 1. theme.json must exist and parse (C-02). The T-052 loader is the same
    // parser the app uses at runtime, so a pack that passes here loads.
    ThemeInfo info;
    if (!ReadThemeInfoFromDir(dir, info)) {
        error = "theme.json missing or unreadable (C-02)";
        return false;
    }
    // 2. Every entry must carry an allowed extension.
    std::vector<std::wstring> pending{dir};
    while (!pending.empty()) {
        const std::wstring current = pending.back();
        pending.pop_back();
        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW((current + L"\\*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) {
            error = "cannot read pack directory";
            return false;
        }
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = current + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                pending.push_back(full);
                continue;
            }
            // Extension = after the last dot in the UTF-8 name.
            const std::string utf8 = WideToUtf8(name);
            const std::size_t dot = utf8.rfind('.');
            const std::string ext =
                dot == std::string::npos ? std::string() : LowerAscii(utf8.substr(dot));
            if (!IsAllowedPackExtension(ext)) {
                error = "pack contains disallowed file: " + utf8;
                DF_WARN(kLogModule, "pack rejected: %s", error.c_str());
                return false;
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    return true;
}

ThemeInstallResult InstallThemePackBytes(const std::string& pack_bytes,
                                         const std::string& expected_sha256_hex,
                                         const std::string& name) {
    ThemeInstallResult result;
    if (!IsValidThemeName(name)) {
        result.error = "invalid theme name '" + name + "'";
        return result;
    }
    if (expected_sha256_hex.empty()) {
        result.error = "no sha256 advertised for this theme; refusing (ADR-0026)";
        return result;
    }

    std::string actual_sha;
    std::string sha_error;
    if (!Sha256Hex(pack_bytes, actual_sha, sha_error)) {
        result.error = "hash failure: " + sha_error;
        return result;
    }
    if (!SameHexCaseInsensitive(actual_sha, expected_sha256_hex)) {
        result.error = "sha256 mismatch (expected " + expected_sha256_hex.substr(0, 12) +
                       "..., got " + actual_sha.substr(0, 12) + "...); refusing install";
        DF_WARN(kLogModule, "install '%s' refused: %s", name.c_str(), result.error.c_str());
        return result;
    }

    const std::wstring themes_dir = ThemesDir();
    const std::wstring dest = themes_dir + L"\\" + Utf8PathToWide(name);
    // Staging lives beside the destination so the final swap is a rename.
    wchar_t tick[32];
    std::swprintf(tick, 32, L"%llu", static_cast<unsigned long long>(GetTickCount64()));
    const std::wstring staging = themes_dir + L"\\" + Utf8PathToWide(name) + L".staging-" +
                                 tick;

    auto fail = [&](std::string reason) {
        RemoveTree(staging);
        result.error = std::move(reason);
        DF_WARN(kLogModule, "install '%s' failed: %s", name.c_str(), result.error.c_str());
        return result;
    };

    EnsureDirectory(themes_dir);
    if (!EnsureDirectory(staging)) {
        return fail("cannot create staging directory");
    }
    std::string unpack_error;
    if (!ExtractThemePackBytes(pack_bytes, staging, unpack_error)) {
        return fail("unpack failed: " + unpack_error);
    }
    std::string validate_error;
    if (!ValidateThemePackDir(staging, validate_error)) {
        return fail("pack rejected: " + validate_error);
    }

    // Replace-or-install: an existing community pack of the same name goes
    // away; a builtin name can never get here (IsValidThemeName refused it).
    if (!RemoveTree(dest)) {
        // RemoveDirectoryW fails when the dir does not exist; that is fine.
        const DWORD err = GetLastError();
        if (err != ERROR_FILE_NOT_FOUND && err != ERROR_PATH_NOT_FOUND) {
            return fail("cannot remove previous install (err=" + std::to_string(err) + ")");
        }
    }
    if (!MoveFileW(staging.c_str(), dest.c_str())) {
        return fail("cannot move staging into place (err=" + std::to_string(GetLastError()) +
                    ")");
    }
    result.ok = true;
    DF_INFO(kLogModule, "installed theme '%s' (%zu bytes, sha %s...)", name.c_str(),
            pack_bytes.size(), actual_sha.substr(0, 12).c_str());
    return result;
}

bool UninstallTheme(const std::string& name, std::string& error) {
    if (!IsValidThemeName(name)) {
        error = "invalid theme name '" + name + "'";
        return false;
    }
    const std::wstring dest = ThemesDir() + L"\\" + Utf8PathToWide(name);
    if (!RemoveTree(dest)) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            error = "theme '" + name + "' is not installed";
            return false;
        }
        error = "delete failed (err=" + std::to_string(err) + ")";
        return false;
    }
    DF_INFO(kLogModule, "uninstalled theme '%s'", name.c_str());
    return true;
}

}  // namespace dockforge::core
