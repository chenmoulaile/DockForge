#include "core/config_reload.h"

#include "core/log.h"
#include "core/paths.h"

#include <windows.h>

namespace dockforge::core {
namespace {

// Reads the whole file with full sharing: the writer (SaveConfig's rename, or a
// text editor) may still hold it. A CRT fopen denies concurrent writers
// (measured on the T-016 hover probe); CreateFileW with all share bits does not.
bool ReadFileText(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (1 << 20)) {
        // A config over 1MB is not a config; refuse before allocating for it.
        CloseHandle(h);
        return false;
    }
    out.assign(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD got = 0;
    const BOOL ok =
        ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(got);
    return true;
}

}  // namespace

ReloadVerdict ReloadConfigFromDisk(const core::Config& current, core::Config& next,
                                   std::string* reason) {
    const std::wstring path = ConfigPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (reason) *reason = "config file missing";
        return ReloadVerdict::Unchanged;
    }

    std::string text;
    if (!ReadFileText(path, text)) {
        if (reason) *reason = "config unreadable";
        return ReloadVerdict::Rejected;
    }

    // ParseConfig never throws and leaves `out` untouched on failure - exactly
    // the rejection semantics C-20 wants, without LoadConfig's side effect of
    // renaming the bad file to config.json.bad (a transient bad read during the
    // writer's flush must not archive the user's file).
    core::Config parsed;
    std::string error;
    if (!ParseConfig(text, parsed, &error)) {
        if (reason) *reason = "invalid JSON: " + error;
        DF_WARN("core", "hot reload rejected the config (%s)", error.c_str());
        return ReloadVerdict::Rejected;
    }

    // Startup clamps too, so a clamped reload is the same "disk truth" the app
    // would have booted with.
    ClampConfig(parsed);

    // Content equality, not byte equality: unknown keys ride along inside
    // unknown_keys_json, so two serialisations differing only in key order are
    // still the same config to the app. Comparing serialised text is the cheap
    // whole-structure comparison (~1KB) and it is what kills the self-save
    // echo loop.
    next = std::move(parsed);
    if (SerializeConfig(next) == SerializeConfig(current)) {
        if (reason) *reason = "content identical";
        return ReloadVerdict::Unchanged;
    }
    return ReloadVerdict::Adopted;
}

}  // namespace dockforge::core
