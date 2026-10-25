// DockForge - optional secret storage for keyed providers (T-075).
#include "core/secrets.h"

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"

namespace dockforge::core {

std::wstring SecretsPath() {
    wchar_t override_path[1024] = {};
    const DWORD written = GetEnvironmentVariableW(L"DOCKFORGE_SECRETS_PATH", override_path,
                                                  static_cast<DWORD>(std::size(override_path)));
    if (written > 0 && written < std::size(override_path)) {
        return std::wstring(override_path, written);
    }
    return ConfigDir() + L"\\secrets.json";
}

std::map<std::string, std::string> ReadSecrets() {
    std::map<std::string, std::string> secrets;
    const std::wstring path = SecretsPath();
    // ReadFile-based helper lives in config.cpp's anonymous namespace; a small
    // local read keeps this module independent of the config parser's file
    // plumbing while using the same Win32 open (std::ifstream has no wchar_t
    // path on MinGW).
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return secrets;  // absent file: no keyed provider, not an error
    }
    std::string data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) && size.QuadPart > 0 && size.QuadPart < (1 << 20)) {
        data.resize(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(handle, data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) {
            read = 0;
        }
        data.resize(read);
    }
    CloseHandle(handle);

    if (data.empty()) return secrets;
    try {
        const nlohmann::json document = nlohmann::json::parse(data);
        if (!document.is_object()) return secrets;
        for (auto it = document.begin(); it != document.end(); ++it) {
            if (it.value().is_string()) {
                secrets[it.key()] = it.value().get<std::string>();
            }
        }
    } catch (const std::exception&) {
        // Malformed secrets degrade to "no key". The path is logged, the
        // contents never are.
        DF_WARN("core", "secrets file at %s is not valid JSON; keyed providers disabled",
                WideToUtf8(path).c_str());
    }
    return secrets;
}

}  // namespace dockforge::core
