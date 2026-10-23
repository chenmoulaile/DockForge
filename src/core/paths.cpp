// DockForge - path + string conversion helpers.
#include "core/paths.h"

#include "platform/win_common.h"

#include <shlobj.h>
#include <iterator>
#include <vector>

namespace dockforge::core {
namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) || !raw) {
        if (raw) CoTaskMemFree(raw);
        return {};
    }
    std::wstring result(raw);
    CoTaskMemFree(raw);
    return result;
}

}  // namespace

std::wstring ConfigDir() {
    static const std::wstring cached = [] {
        std::wstring base = KnownFolder(FOLDERID_RoamingAppData);
        if (base.empty()) return std::wstring(L"DockForge");
        return base + L"\\DockForge";
    }();
    return cached;
}

std::wstring ConfigPath() {
    static const std::wstring cached = [] {
        // Environment override first. Tests and evidence probes need a config they can
        // freely overwrite; without this, a unit test that exercises the serializer
        // writes the developer's real dock order (measured, see paths.h).
        wchar_t override_path[1024] = {};
        const DWORD written = GetEnvironmentVariableW(L"DOCKFORGE_CONFIG_PATH", override_path,
                                                      static_cast<DWORD>(std::size(override_path)));
        if (written > 0 && written < std::size(override_path)) {
            return std::wstring(override_path, written);
        }
        return ConfigDir() + L"\\config.json";
    }();
    return cached;
}


std::wstring ThemesDir() { return ConfigDir() + L"\\themes"; }

std::wstring LogsDir() {
    static const std::wstring cached = [] {
        std::wstring base = KnownFolder(FOLDERID_LocalAppData);
        if (base.empty()) return std::wstring(L"DockForge\\logs");
        return base + L"\\DockForge\\logs";
    }();
    return cached;
}

std::wstring DumpsDir() {
    std::wstring base = KnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) return L"DockForge\\dumps";
    return base + L"\\DockForge\\dumps";
}

std::wstring IconCacheDir() {
    static const std::wstring cached = [] {
        std::wstring base = KnownFolder(FOLDERID_LocalAppData);
        if (base.empty()) return std::wstring(L"DockForge\\icons");
        return base + L"\\DockForge\\icons";
    }();
    return cached;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    const DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;

    // Parent may be missing: create ancestors one level at a time.
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return false;
    if (!EnsureDirectory(path.substr(0, slash))) return false;

    if (CreateDirectoryW(path.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring ExecutableDir() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) return {};
        if (written < buffer.size() - 1) {
            std::wstring path(buffer.data(), written);
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

}  // namespace dockforge::core
