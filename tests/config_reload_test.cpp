// Tests for the config hot-reload decision (T-051, CONTRACTS.md C-20).
//
// The debounce itself is LaunchpadWatcher's DebounceGate, already covered by the
// launchpad watcher tests (T-040 防抖三例). What is new here - and what silently
// breaks user data when wrong - is the reload decision: reject invalid JSON
// without archiving the user's file, short-circuit content-identical saves so
// the watcher never echoes the app's own write into a publish loop, and clamp
// out-of-range values the same way boot does.
#include <catch_amalgamated.hpp>

#include <windows.h>

#include <string>

#include "core/config.h"
#include "core/config_reload.h"
#include "core/paths.h"

namespace core = dockforge::core;

namespace {

// Writes text to the isolated config path (tests/main_test.cpp points
// DOCKFORGE_CONFIG_PATH at a throwaway file before any test runs).
bool WriteConfigFile(const std::string& text) {
    const std::wstring path = core::ConfigPath();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    return ok && written == text.size();
}

}  // namespace

TEST_CASE("A real change is adopted and visible in next", "[config-reload]") {
    core::Config current;
    REQUIRE(WriteConfigFile(R"({"dock": {"icon_size": 64, "gap": 10}})"));

    core::Config next;
    std::string reason;
    const core::ReloadVerdict verdict = core::ReloadConfigFromDisk(current, next, &reason);
    REQUIRE(verdict == core::ReloadVerdict::Adopted);
    CHECK(next.dock.icon_size == 64);
    CHECK(next.dock.gap == 10);
}

TEST_CASE("A content-identical file is Unchanged, not a publish trigger", "[config-reload]") {
    // The settings window saves and the watcher sees config.json change; without
    // this branch the app would re-publish its own write forever.
    core::Config current;
    current.dock.icon_size = 56;
    REQUIRE(core::SaveConfig(current));  // disk now serialises to exactly `current`

    core::Config next;
    std::string reason;
    const core::ReloadVerdict verdict = core::ReloadConfigFromDisk(current, next, &reason);
    CHECK(verdict == core::ReloadVerdict::Unchanged);
}

TEST_CASE("Invalid JSON is rejected and the running config survives", "[config-reload]") {
    core::Config current;
    current.dock.icon_size = 48;
    const std::string broken = "{\"dock\": {\"icon_size\": ";

    REQUIRE(WriteConfigFile(broken));
    core::Config next;
    std::string reason;
    const core::ReloadVerdict verdict = core::ReloadConfigFromDisk(current, next, &reason);
    REQUIRE(verdict == core::ReloadVerdict::Rejected);
    CHECK_FALSE(reason.empty());
    // `next` must not be adopted by the caller; assert it was not touched into
    // a usable-but-wrong shape either.
    CHECK(next.dock.icon_size == core::Config{}.dock.icon_size);

    // The user's file is left on disk exactly as written - LoadConfig's
    // rename-to-.bad side effect must not fire from a reload (the writer may
    // simply not have flushed yet).
    std::wstring path = core::ConfigPath();
    CHECK(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);
    // And it is still the same broken text, so a completed write can be read.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    char buffer[256]{};
    DWORD got = 0;
    ReadFile(h, buffer, sizeof(buffer) - 1, &got, nullptr);
    CloseHandle(h);
    CHECK(std::string(buffer, got) == broken);
}

TEST_CASE("Out-of-range values are clamped and adopted like boot does", "[config-reload]") {
    core::Config current;
    REQUIRE(WriteConfigFile(R"({"dock": {"icon_size": 9999}})"));

    core::Config next;
    std::string reason;
    const core::ReloadVerdict verdict = core::ReloadConfigFromDisk(current, next, &reason);
    REQUIRE(verdict == core::ReloadVerdict::Adopted);
    CHECK(next.dock.icon_size == core::IconSizeRange().max);
}

TEST_CASE("A partial file fills the rest with defaults", "[config-reload]") {
    core::Config current;
    REQUIRE(WriteConfigFile(R"({"dock": {"gap": 12}})"));

    core::Config next;
    REQUIRE(core::ReloadConfigFromDisk(current, next, nullptr) ==
            core::ReloadVerdict::Adopted);
    CHECK(next.dock.gap == 12);
    // Everything the file did not mention is the boot default, not garbage.
    CHECK(next.dock.icon_size == core::Config{}.dock.icon_size);
    CHECK(next.launchpad.page_size == core::Config{}.launchpad.page_size);
}

TEST_CASE("A missing file is Unchanged, not an adoption of defaults", "[config-reload]") {
    core::Config current;
    current.dock.icon_size = 72;
    // The isolated path does not exist until something writes it; make sure.
    DeleteFileW(core::ConfigPath().c_str());

    core::Config next;
    std::string reason;
    CHECK(core::ReloadConfigFromDisk(current, next, &reason) ==
          core::ReloadVerdict::Unchanged);
}
