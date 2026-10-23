// T-001 smoke tests: the build chain produces a library whose core helpers
// behave predictably. Deeper suites arrive with T-002 (config/log) and
// T-008 (layout math).
#include "catch_amalgamated.hpp"

#include "core/paths.h"

#include <windows.h>

#include <iterator>
#include <string>

TEST_CASE("paths: config path honours the environment override", "[paths]") {
    // tests/main_test.cpp points DOCKFORGE_CONFIG_PATH at a temp directory during
    // static init, because this binary shares a profile with the product and a test
    // that saved the config rewrote the developer's real dock arrangement.
    //
    // So the assertion is about the override, not about the default location: the
    // default is covered by ConfigDir below, and asserting "ConfigPath lives under
    // ConfigDir" here would now be false by design.
    wchar_t raw[1024] = {};
    const DWORD written = GetEnvironmentVariableW(L"DOCKFORGE_CONFIG_PATH", raw,
                                                  static_cast<DWORD>(std::size(raw)));
    REQUIRE(written > 0);

    const std::wstring file = dockforge::core::ConfigPath();
    REQUIRE(file == std::wstring(raw, written));
    REQUIRE(file.substr(file.size() - 11) == L"config.json");
}

TEST_CASE("paths: config directory lives under DockForge", "[paths]") {
    const std::wstring dir = dockforge::core::ConfigDir();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(dir.substr(dir.size() - 9) == L"DockForge");
}

TEST_CASE("paths: utf8 round trip survives non-ascii", "[paths]") {
    const std::wstring original = L"DockForge \u4e2d\u6587 \u00e9\u00e8";
    const std::string utf8 = dockforge::core::WideToUtf8(original);
    REQUIRE_FALSE(utf8.empty());
    REQUIRE(dockforge::core::Utf8ToWide(utf8) == original);
}

TEST_CASE("paths: utf8 helpers tolerate empty input", "[paths]") {
    REQUIRE(dockforge::core::Utf8ToWide("").empty());
    REQUIRE(dockforge::core::WideToUtf8(L"").empty());
}
