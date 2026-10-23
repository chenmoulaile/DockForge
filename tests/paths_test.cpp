// T-001 smoke tests: the build chain produces a library whose core helpers
// behave predictably. Deeper suites arrive with T-002 (config/log) and
// T-008 (layout math).
#include "catch_amalgamated.hpp"

#include "core/paths.h"

#include <string>

TEST_CASE("paths: config path lives under DockForge", "[paths]") {
    const std::wstring dir = dockforge::core::ConfigDir();
    const std::wstring file = dockforge::core::ConfigPath();
    REQUIRE_FALSE(dir.empty());
    REQUIRE(file.size() > dir.size());
    REQUIRE(file.rfind(dir, 0) == 0);
    REQUIRE(file.substr(file.size() - 11) == L"config.json");
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
