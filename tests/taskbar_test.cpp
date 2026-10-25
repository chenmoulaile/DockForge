// T-061 unit tests: the taskbar visibility decision layer and the emergency
// restore semantics.
//
// The real Shell_Traywnd backend is exercised for real by tools/taskbar_probe.ps1
// on the live desktop; these tests run the decision rules against a fake so they
// stay hermetic - hiding the actual taskbar from the test binary would be exactly
// the developer-machine mutation the config-path isolation convention exists to
// prevent.
#include "catch_amalgamated.hpp"

#include "core/taskbar.h"

#include <string>

namespace {

using dockforge::core::ApplyTaskbarVisibility;
using dockforge::core::TaskbarBackend;

// A backend that records what the decision layer asked for and keeps a
// visibility state the way the real shell would.
struct FakeTaskbar : TaskbarBackend {
    bool window_exists = true;
    bool visible = true;
    bool hide_result = true;
    bool show_result = true;
    int hides = 0;
    int shows = 0;
    int visible_queries = 0;

    bool WindowExists() override { return window_exists; }
    bool IsVisible() override {
        ++visible_queries;
        return visible;
    }
    bool Hide() override {
        ++hides;
        if (hide_result) visible = false;
        return hide_result;
    }
    bool Show() override {
        ++shows;
        if (show_result) visible = true;
        return show_result;
    }
};

}  // namespace

TEST_CASE("apply taskbar: hide with a visible taskbar issues exactly one Hide",
          "[taskbar]") {
    FakeTaskbar backend;
    std::string detail;
    REQUIRE(ApplyTaskbarVisibility(true, backend, &detail));
    REQUIRE(backend.hides == 1);
    REQUIRE(backend.shows == 0);
    REQUIRE(backend.visible == false);
    REQUIRE(detail == "hidden");
}

TEST_CASE("apply taskbar: hide when already hidden is a success no-op", "[taskbar]") {
    FakeTaskbar backend;
    backend.visible = false;
    std::string detail;
    REQUIRE(ApplyTaskbarVisibility(true, backend, &detail));
    REQUIRE(backend.hides == 0);
    REQUIRE(backend.shows == 0);
    REQUIRE(detail == "already hidden");
}

TEST_CASE("apply taskbar: show with a hidden taskbar restores", "[taskbar]") {
    FakeTaskbar backend;
    backend.visible = false;
    std::string detail;
    REQUIRE(ApplyTaskbarVisibility(false, backend, &detail));
    REQUIRE(backend.shows == 1);
    REQUIRE(backend.hides == 0);
    REQUIRE(backend.visible == true);
    REQUIRE(detail == "restored");
}

TEST_CASE("apply taskbar: show when already visible is a success no-op", "[taskbar]") {
    FakeTaskbar backend;
    std::string detail;
    REQUIRE(ApplyTaskbarVisibility(false, backend, &detail));
    REQUIRE(backend.shows == 0);
    REQUIRE(backend.hides == 0);
    REQUIRE(detail == "already visible");
}

TEST_CASE("apply taskbar: hide with the window absent is a success no-op (explorer "
          "restarting; the replacement comes back visible)",
          "[taskbar]") {
    FakeTaskbar backend;
    backend.window_exists = false;
    std::string detail;
    REQUIRE(ApplyTaskbarVisibility(true, backend, &detail));
    REQUIRE(backend.hides == 0);
    REQUIRE(detail == "taskbar window not found (explorer restarting?)");
}

TEST_CASE("apply taskbar: repeated identical applies are idempotent", "[taskbar]") {
    FakeTaskbar backend;
    std::string detail;
    REQUIRE(ApplyTaskbarVisibility(true, backend, &detail));
    REQUIRE(ApplyTaskbarVisibility(true, backend, &detail));
    REQUIRE(backend.hides == 1);  // only the transition acted
    REQUIRE(ApplyTaskbarVisibility(false, backend, &detail));
    REQUIRE(ApplyTaskbarVisibility(false, backend, &detail));
    REQUIRE(backend.shows == 1);
}

TEST_CASE("apply taskbar: a failing Hide is reported as failed", "[taskbar]") {
    FakeTaskbar backend;
    backend.hide_result = false;
    std::string detail;
    REQUIRE_FALSE(ApplyTaskbarVisibility(true, backend, &detail));
    REQUIRE(detail == "hide FAILED");
}

TEST_CASE("apply taskbar: a failing Show is reported as failed", "[taskbar]") {
    FakeTaskbar backend;
    backend.visible = false;
    backend.show_result = false;
    std::string detail;
    REQUIRE_FALSE(ApplyTaskbarVisibility(false, backend, &detail));
    REQUIRE(detail == "restore FAILED");
}

TEST_CASE("apply taskbar: null detail pointer is accepted", "[taskbar]") {
    FakeTaskbar backend;
    REQUIRE(ApplyTaskbarVisibility(true, backend, nullptr));
    REQUIRE(backend.hides == 1);
}

TEST_CASE("restore: the emergency path shows even when the state reads visible",
          "[taskbar]") {
    // The forced restore must not consult IsVisible and must not trust a stale
    // read: it always issues Show. ShowWindowAsync on a visible window is
    // harmless; skipping the show on a stale "visible" would strand the user.
    FakeTaskbar backend;
    std::string detail;
    REQUIRE(dockforge::core::ForceShowTaskbar(backend, &detail));
    REQUIRE(backend.shows == 1);
    REQUIRE(backend.visible_queries == 0);  // forced: the state was never asked
    REQUIRE(detail == "restored (forced show)");
}

TEST_CASE("restore: forced show with the window absent is a success no-op", "[taskbar]") {
    FakeTaskbar backend;
    backend.window_exists = false;
    std::string detail;
    REQUIRE(dockforge::core::ForceShowTaskbar(backend, &detail));
    REQUIRE(backend.shows == 0);
    REQUIRE(detail == "taskbar window not found (explorer restarting?)");
}

TEST_CASE("restore: a failed forced show is reported as failed", "[taskbar]") {
    FakeTaskbar backend;
    backend.show_result = false;
    std::string detail;
    REQUIRE_FALSE(dockforge::core::ForceShowTaskbar(backend, &detail));
    REQUIRE(detail == "restore FAILED (no settle)");
}
