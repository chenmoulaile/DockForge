// T-065 unit tests: the C-25 adapter framework (registration, throttling,
// change notification, silent degradation) and the title-badge parse.
//
// The real window scan is exercised once for real against the live desktop
// (TitleBadgeAdapter with a process name nothing answers to), everything else
// runs against fakes so the host rules stay hermetic.
#include "catch_amalgamated.hpp"

#include "core/config.h"
#include "shell/app_entry.h"
#include "shell/message_counts.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

using dockforge::shell::IMessageCounterAdapter;
using dockforge::shell::MessageCounterHost;
using dockforge::shell::ParseUnreadFromTitle;
using dockforge::shell::TitleBadgeAdapter;

namespace {

// Records every host interaction; the count is whatever the test sets.
struct FakeAdapter : IMessageCounterAdapter {
    std::wstring id;
    int count = 0;
    bool init_result = true;
    int inits = 0;
    int shutdowns = 0;
    int polls = 0;

    explicit FakeAdapter(std::wstring id_) : id(std::move(id_)) {}

    const std::wstring& TargetAppId() const override { return id; }
    bool Initialize() override {
        ++inits;
        return init_result;
    }
    void Shutdown() override { ++shutdowns; }
    int CurrentCount() override {
        ++polls;
        return count;
    }
};

// Manual clock: the host throttle is advanced by moving `now` forward.
struct ManualClock {
    double now_ms = 0.0;
    MessageCounterHost::ClockFn fn() {
        return [this] { return now_ms; };
    }
};

}  // namespace

TEST_CASE("ParseUnreadFromTitle reads chat-app title badges", "[t065][title]") {
    // WeChat Win32 decorates the prefix, QQ variants the suffix.
    CHECK(ParseUnreadFromTitle(L"(12) 微信") == 12);
    CHECK(ParseUnreadFromTitle(L"WeChat (3)") == 3);
    CHECK(ParseUnreadFromTitle(L"(5) QQ") == 5);
    // No badge at all.
    CHECK(ParseUnreadFromTitle(L"微信") == 0);
    CHECK(ParseUnreadFromTitle(L"") == 0);
    // Explicit zero stays zero - the caller draws nothing for it.
    CHECK(ParseUnreadFromTitle(L"(0) 微信") == 0);
    // Highest group wins; non-numeric groups are ignored.
    CHECK(ParseUnreadFromTitle(L"(1) (7) 项目讨论") == 7);
    CHECK(ParseUnreadFromTitle(L"投诉(abc)") == 0);
    // A parse bug must not paint a four-digit badge.
    CHECK(ParseUnreadFromTitle(L"(99999) 微信") == 999);
    // Unbalanced parenthesis is not a badge.
    CHECK(ParseUnreadFromTitle(L"(12 微信") == 0);
}

TEST_CASE("MessageCounterHost register/unregister keeps one adapter per id",
          "[t065][host]") {
    MessageCounterHost host(1000, ManualClock{}.fn());

    CHECK(host.Register(std::make_unique<FakeAdapter>(L"weixin.exe")));
    CHECK(host.AdapterCount() == 1);

    // C-25: a second adapter under the same TargetAppId is refused, first wins.
    CHECK_FALSE(host.Register(std::make_unique<FakeAdapter>(L"weixin.exe")));
    CHECK(host.AdapterCount() == 1);

    CHECK(host.Register(std::make_unique<FakeAdapter>(L"qq.exe")));
    CHECK(host.AdapterCount() == 2);

    CHECK(host.CountFor(L"weixin.exe") == 0);  // registered but not initialized yet
    CHECK(host.CountFor(L"unknown") == -1);    // no adapter at all

    CHECK(host.Unregister(L"weixin.exe"));
    CHECK_FALSE(host.Unregister(L"weixin.exe"));  // already gone
    CHECK(host.AdapterCount() == 1);
    CHECK(host.CountFor(L"weixin.exe") == -1);
}

TEST_CASE("MessageCounterHost reports counts only on change", "[t065][host]") {
    ManualClock clock;
    MessageCounterHost host(1000, clock.fn());

    auto* fake = new FakeAdapter(L"weixin.exe");
    fake->count = 5;
    CHECK(host.Register(std::unique_ptr<IMessageCounterAdapter>(fake)));

    std::map<std::wstring, int> events;
    host.set_count_changed([&](const std::wstring& id, int count) { events[id] = count; });

    // Update 1: initializes (no poll in the same pass), Update 2: first poll.
    host.Update();
    clock.now_ms = 2000;
    CHECK(host.Update());
    CHECK(events[L"weixin.exe"] == 5);
    CHECK(host.CountFor(L"weixin.exe") == 5);
    CHECK(fake->polls == 1);

    // Same count on the next (throttled-out) tick: no event, no poll.
    clock.now_ms = 2500;
    CHECK_FALSE(host.Update());
    CHECK(fake->polls == 1);

    // New tick inside the throttle window (only 900ms since the poll): no event,
    // no poll.
    clock.now_ms = 2900;
    CHECK_FALSE(host.Update());
    CHECK(fake->polls == 1);

    // Past the interval, same count: polled again, still no event.
    fake->count = 9;
    clock.now_ms = 5000;
    CHECK(host.Update());
    CHECK(events[L"weixin.exe"] == 9);
    CHECK(fake->polls == 2);
}

TEST_CASE("MessageCounterHost zeroes the count when the target process exits",
          "[t065][host]") {
    ManualClock clock;
    MessageCounterHost host(1000, clock.fn());

    auto* fake = new FakeAdapter(L"weixin.exe");
    fake->count = 4;
    CHECK(host.Register(std::unique_ptr<IMessageCounterAdapter>(fake)));
    host.Update();
    clock.now_ms = 2000;
    (void)host.Update();
    REQUIRE(host.CountFor(L"weixin.exe") == 4);

    // The process is gone: the adapter answers 0 (real TitleBadgeAdapter does,
    // because nothing matches the process name any more) and the host must fire
    // exactly one CountChanged(0) and keep answering 0 afterwards.
    fake->count = 0;
    clock.now_ms = 4000;
    CHECK(host.Update());
    CHECK(host.CountFor(L"weixin.exe") == 0);
    clock.now_ms = 6000;
    CHECK_FALSE(host.Update());  // no repeat events while it stays at 0
    CHECK(host.CountFor(L"weixin.exe") == 0);
}

TEST_CASE("MessageCounterHost skips adapters whose Initialize fails, once",
          "[t065][host]") {
    ManualClock clock;
    MessageCounterHost host(1000, clock.fn());

    auto* broken = new FakeAdapter(L"broken.app");
    broken->init_result = false;
    CHECK(host.Register(std::unique_ptr<IMessageCounterAdapter>(broken)));

    for (int i = 0; i < 3; ++i) {
        clock.now_ms += 2000;
        (void)host.Update();
    }
    // C-25: one WARN, never retried, never polled.
    CHECK(broken->inits == 1);
    CHECK(broken->polls == 0);
    CHECK(host.CountFor(L"broken.app") == 0);  // degraded, not -1
}

TEST_CASE("MessageCounterHost honors the per-adapter switch", "[t065][host]") {
    ManualClock clock;
    MessageCounterHost host(1000, clock.fn());

    auto* fake = new FakeAdapter(L"weixin.exe");
    fake->count = 3;
    CHECK(host.Register(std::unique_ptr<IMessageCounterAdapter>(fake)));
    host.Update();
    clock.now_ms = 2000;
    (void)host.Update();
    REQUIRE(host.CountFor(L"weixin.exe") == 3);

    std::map<std::wstring, int> events;
    host.set_count_changed([&](const std::wstring& id, int count) { events[id] = count; });

    // Off: shutdown + one zero event + CountFor answers 0.
    host.SetEnabled(L"weixin.exe", false);
    CHECK(fake->shutdowns == 1);
    CHECK(events[L"weixin.exe"] == 0);
    CHECK(host.CountFor(L"weixin.exe") == 0);

    // While off the adapter is not polled even on fresh ticks.
    fake->count = 7;
    clock.now_ms = 4000;
    CHECK_FALSE(host.Update());
    CHECK(fake->polls == 1);

    // On again: re-initialized on the next Update (that pass does not poll),
    // and the new count lands on the tick after it.
    host.SetEnabled(L"weixin.exe", true);
    clock.now_ms = 6000;
    (void)host.Update();
    clock.now_ms = 8000;
    CHECK(host.Update());
    CHECK(events[L"weixin.exe"] == 7);
    CHECK(host.CountFor(L"weixin.exe") == 7);
}

TEST_CASE("CountForEntry matches tiles by executable basename or AUMID", "[t065][host]") {
    ManualClock clock;
    MessageCounterHost host(1000, clock.fn());

    auto* weixin = new FakeAdapter(L"weixin.exe");
    weixin->count = 2;
    CHECK(host.Register(std::unique_ptr<IMessageCounterAdapter>(weixin)));
    host.Update();
    clock.now_ms = 2000;
    (void)host.Update();

    dockforge::shell::AppEntry win32;
    win32.target_path = L"D:\\Weixin\\Weixin.exe";
    CHECK(host.CountForEntry(win32) == 2);

    dockforge::shell::AppEntry other;
    other.target_path = L"C:\\Program Files\\7-Zip\\7zFM.exe";
    CHECK(host.CountForEntry(other) == 0);

    dockforge::shell::AppEntry packaged;
    packaged.aumid = L"Weixin.exe";  // an AUMID could collide with an id by design
    CHECK(host.CountForEntry(packaged) == 2);

    // Both identities on one tile, only the path matches: the second shape must
    // still be tried when the AUMID finds no adapter.
    dockforge::shell::AppEntry mixed;
    mixed.aumid = L"SomeVendor.Package!App";
    mixed.target_path = L"D:\\Weixin\\Weixin.exe";
    CHECK(host.CountForEntry(mixed) == 2);

    dockforge::shell::AppEntry neither;
    CHECK(host.CountForEntry(neither) == 0);
}

TEST_CASE("TitleBadgeAdapter answers 0 when the target is not running", "[t065][adapter]") {
    // Real scan against the live desktop. The process name belongs to nothing,
    // so every window is skipped and the count must degrade to 0 without
    // leaking handles or exceptions - the exact behaviour the contract demands
    // when the target exits.
    TitleBadgeAdapter gone(L"no.such.process.exe", {L"dockforge_no_such_app.exe"});
    CHECK(gone.Initialize());
    CHECK(gone.CurrentCount() == 0);
    gone.Shutdown();
}

TEST_CASE("integrations.message_counts round-trips through the config", "[t065][config]") {
    dockforge::core::Config config;
    config.used_defaults = false;
    config.integrations.message_counts["weixin.exe"] = false;
    config.integrations.message_counts["qq.exe"] = true;

    const std::string text = dockforge::core::SerializeConfig(config);
    CHECK(text.find("integrations") != std::string::npos);
    CHECK(text.find("message_counts") != std::string::npos);

    dockforge::core::Config parsed;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig(text, parsed, &error));
    REQUIRE(parsed.integrations.message_counts.size() == 2);
    CHECK(parsed.integrations.message_counts.at("weixin.exe") == false);
    CHECK(parsed.integrations.message_counts.at("qq.exe") == true);

    // Unknown ids survive so a downgrade cannot corrupt the user's file.
    dockforge::core::Config raw;
    REQUIRE(dockforge::core::ParseConfig(
        R"({"integrations":{"message_counts":{"future.app.enabled":false}}})", raw, &error));
    REQUIRE(raw.integrations.message_counts.size() == 1);
    CHECK(raw.integrations.message_counts.at("future.app.enabled") == false);
    const std::string rewritten = dockforge::core::SerializeConfig(raw);
    CHECK(rewritten.find("future.app.enabled") != std::string::npos);

    // A non-boolean switch is dropped with a rewrite flag, not silently kept.
    dockforge::core::Config bad;
    REQUIRE(dockforge::core::ParseConfig(
        R"({"integrations":{"message_counts":{"weixin.exe":"yes"}}})", bad, &error));
    CHECK(bad.needs_rewrite);
}
