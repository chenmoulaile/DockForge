// T-060 unit tests: the autostart decision layer and XML helpers.
//
// The real COM backend is exercised for real by tools/autostart_probe.ps1;
// these tests run the decision rules against a fake so they stay hermetic -
// creating or deleting the actual scheduled task from the test binary would
// mutate developer-machine state exactly like the config-path incident that
// motivated tests/main_test.cpp's isolation.
#include "catch_amalgamated.hpp"

#include "core/autostart.h"
#include "core/config.h"

#include <string>

namespace {

using dockforge::core::ApplyAutostart;
using dockforge::core::AutostartBackend;

// A backend that records what the decision layer asked for.
struct FakeBackend : AutostartBackend {
    bool exists = false;
    bool create_result = true;
    bool remove_result = true;
    std::wstring created_with;
    int creates = 0;
    int removes = 0;
    int exists_queries = 0;

    bool Exists() override {
        ++exists_queries;
        return exists;
    }
    bool Create(const std::wstring& exe_path) override {
        ++creates;
        created_with = exe_path;
        exists = create_result;  // a successful register implies presence
        return create_result;
    }
    bool Remove() override {
        ++removes;
        if (remove_result) exists = false;
        return remove_result;
    }
};

}  // namespace

TEST_CASE("autostart task name is fixed", "[autostart]") {
    REQUIRE(dockforge::core::AutostartTaskName() == L"\\DockForge");
}

TEST_CASE("autostart xml escape covers the five predefined entities",
          "[autostart]") {
    const std::string escaped = dockforge::core::AutostartXmlEscape(
        R"(a&b<c>d"e'f)");
    REQUIRE(escaped == "a&amp;b&lt;c&gt;d&quot;e&apos;f");
    // Plain text passes through untouched.
    REQUIRE(dockforge::core::AutostartXmlEscape("C:\\Apps\\DockForge.exe") ==
            "C:\\Apps\\DockForge.exe");
}

TEST_CASE("apply autostart: enable with absent task registers once",
          "[autostart]") {
    FakeBackend backend;
    std::string detail;
    const bool ok =
        ApplyAutostart(true, backend, L"C:\\Apps\\DockForge.exe", &detail);
    REQUIRE(ok);
    REQUIRE(backend.creates == 1);
    REQUIRE(backend.removes == 0);
    REQUIRE(backend.created_with == L"C:\\Apps\\DockForge.exe");
    REQUIRE(detail == "task registered (logon trigger)");
}

TEST_CASE("apply autostart: enable with present task re-registers (moved binary)",
          "[autostart]") {
    FakeBackend backend;
    backend.exists = true;
    std::string detail;
    const bool ok = ApplyAutostart(true, backend, L"D:\\New\\DockForge.exe", &detail);
    REQUIRE(ok);
    REQUIRE(backend.creates == 1);
    REQUIRE(backend.created_with == L"D:\\New\\DockForge.exe");
    REQUIRE(detail == "task registered (logon trigger)");
}

TEST_CASE("apply autostart: disable with present task deletes", "[autostart]") {
    FakeBackend backend;
    backend.exists = true;
    std::string detail;
    const bool ok = ApplyAutostart(false, backend, L"C:\\Apps\\DockForge.exe", &detail);
    REQUIRE(ok);
    REQUIRE(backend.removes == 1);
    REQUIRE(backend.creates == 0);
    REQUIRE(detail == "task deleted");
}

TEST_CASE("apply autostart: disable with absent task is an idempotent no-op",
          "[autostart]") {
    FakeBackend backend;
    std::string detail;
    const bool ok = ApplyAutostart(false, backend, L"C:\\Apps\\DockForge.exe", &detail);
    REQUIRE(ok);
    REQUIRE(backend.removes == 0);
    REQUIRE(backend.creates == 0);
    // The DoD wording "关闭时删除任务，不残留" is satisfied vacuously here: the
    // decision layer must not delete anything when nothing exists (and must not
    // fail either - the end state the user asked for is already in place).
    REQUIRE(detail == "already off (task absent)");
}

TEST_CASE("apply autostart: backend failure is reported, not swallowed",
          "[autostart]") {
    SECTION("create failure") {
        FakeBackend backend;
        backend.create_result = false;
        std::string detail;
        REQUIRE_FALSE(ApplyAutostart(true, backend, L"exe", &detail));
        REQUIRE(detail == "register FAILED");
    }
    SECTION("delete failure") {
        FakeBackend backend;
        backend.exists = true;
        backend.remove_result = false;
        std::string detail;
        REQUIRE_FALSE(ApplyAutostart(false, backend, L"exe", &detail));
        REQUIRE(detail == "delete FAILED");
    }
}

TEST_CASE("config default keeps autostart off (T-060 DoD)", "[autostart]") {
    // The loader only reads disk in LoadConfig; ParseConfig on empty JSON is
    // the hermetic way to see the schema defaults (C-01: behavior.autostart
    // default false, and the settings model already echoes it back as "0").
    dockforge::core::Config config;
    REQUIRE(ParseConfig("{}", config, nullptr));
    REQUIRE(config.behavior.autostart == false);
    REQUIRE(config.behavior.hide_taskbar == false);

    // And an explicit true round-trips through the serializer without being
    // clamped, renamed, or dropped.
    config.behavior.autostart = true;
    const std::string json = SerializeConfig(config);
    dockforge::core::Config loaded;
    REQUIRE(ParseConfig(json, loaded, nullptr));
    REQUIRE(loaded.behavior.autostart == true);
}
