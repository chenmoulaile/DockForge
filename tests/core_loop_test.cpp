// T-003: event bus semantics + frame clock statistics.
#include "catch_amalgamated.hpp"

#include "core/app_loop.h"
#include "core/event_bus.h"
#include "core/frame_clock.h"
#include "platform/win_common.h"

#include <atomic>
#include <string>
#include <vector>

using namespace dockforge::core;

namespace {
struct PingEvent {
    int value = 0;
};
struct OtherEvent {
    std::string text;
};
}  // namespace

TEST_CASE("event bus: delivers to every subscriber in order", "[events]") {
    EventBus bus;
    std::vector<int> seen;

    bus.Subscribe<PingEvent>([&](const PingEvent& e) { seen.push_back(e.value * 10); });
    bus.Subscribe<PingEvent>([&](const PingEvent& e) { seen.push_back(e.value * 20); });

    bus.Publish(PingEvent{3});
    REQUIRE(seen == std::vector<int>{30, 60});
    REQUIRE(bus.HandlerCount<PingEvent>() == 2);
}

TEST_CASE("event bus: event types are isolated from each other", "[events]") {
    EventBus bus;
    int pings = 0;
    int others = 0;
    bus.Subscribe<PingEvent>([&](const PingEvent&) { ++pings; });
    bus.Subscribe<OtherEvent>([&](const OtherEvent&) { ++others; });

    bus.Publish(PingEvent{1});
    REQUIRE(pings == 1);
    REQUIRE(others == 0);

    bus.Publish(OtherEvent{"hello"});
    REQUIRE(pings == 1);
    REQUIRE(others == 1);
}

TEST_CASE("event bus: unsubscribed handler stops receiving events", "[events]") {
    EventBus bus;
    int calls = 0;
    const auto id = bus.Subscribe<PingEvent>([&](const PingEvent&) { ++calls; });

    bus.Publish(PingEvent{});
    REQUIRE(calls == 1);

    bus.Unsubscribe(id);
    bus.Publish(PingEvent{});
    REQUIRE(calls == 1);
    REQUIRE(bus.HandlerCount<PingEvent>() == 0);

    bus.Unsubscribe(id);  // double unsubscribe is a no-op
    REQUIRE(bus.HandlerCount<PingEvent>() == 0);
}

TEST_CASE("event bus: handler may unsubscribe itself during dispatch", "[events]") {
    EventBus bus;
    int calls = 0;
    EventBus::SubscriptionId self = 0;
    self = bus.Subscribe<PingEvent>([&](const PingEvent&) {
        ++calls;
        bus.Unsubscribe(self);
    });
    bus.Subscribe<PingEvent>([&](const PingEvent&) { ++calls; });

    bus.Publish(PingEvent{});
    // Both handlers still run for the in-flight event; the removal is deferred.
    REQUIRE(calls == 2);
    REQUIRE(bus.HandlerCount<PingEvent>() == 1);

    bus.Publish(PingEvent{});
    REQUIRE(calls == 3);
}

TEST_CASE("event bus: handler may unsubscribe another handler mid-dispatch", "[events]") {
    EventBus bus;
    int first_calls = 0;
    int second_calls = 0;
    EventBus::SubscriptionId second = 0;

    bus.Subscribe<PingEvent>([&](const PingEvent&) {
        ++first_calls;
        bus.Unsubscribe(second);
    });
    second = bus.Subscribe<PingEvent>([&](const PingEvent&) { ++second_calls; });

    bus.Publish(PingEvent{});
    REQUIRE(first_calls == 1);
    // The removal took effect immediately: the second handler never got its turn.
    REQUIRE(second_calls == 0);
    REQUIRE(bus.HandlerCount<PingEvent>() == 1);

    bus.Publish(PingEvent{});
    REQUIRE(second_calls == 0);
}

TEST_CASE("event bus: subscribing during dispatch does not fire for the in-flight event",
          "[events]") {
    EventBus bus;
    int late_calls = 0;
    bool added = false;

    bus.Subscribe<PingEvent>([&](const PingEvent&) {
        if (added) return;
        added = true;
        bus.Subscribe<PingEvent>([&](const PingEvent&) { ++late_calls; });
    });

    bus.Publish(PingEvent{});
    REQUIRE(late_calls == 0);
    REQUIRE(bus.HandlerCount<PingEvent>() == 2);

    bus.Publish(PingEvent{});
    REQUIRE(late_calls == 1);
}

TEST_CASE("event bus: nested publish is re-entrant", "[events]") {
    EventBus bus;
    int outer = 0;
    int inner = 0;
    bus.Subscribe<PingEvent>([&](const PingEvent& e) {
        if (e.value == 1) {
            ++outer;
            bus.Publish(PingEvent{2});
        } else {
            ++inner;
        }
    });

    bus.Publish(PingEvent{1});
    REQUIRE(outer == 1);
    REQUIRE(inner == 1);
}

TEST_CASE("event bus: Clear drops everything", "[events]") {
    EventBus bus;
    int calls = 0;
    bus.Subscribe<PingEvent>([&](const PingEvent&) { ++calls; });
    bus.Subscribe<OtherEvent>([&](const OtherEvent&) { ++calls; });
    REQUIRE(bus.TotalHandlerCount() == 2);

    bus.Clear();
    REQUIRE(bus.TotalHandlerCount() == 0);
    bus.Publish(PingEvent{});
    REQUIRE(calls == 0);
}

TEST_CASE("frame clock: percentile matches a hand-computed distribution", "[perf]") {
    FrameClock clock(100);
    for (int i = 1; i <= 100; ++i) clock.AddSample(static_cast<double>(i));

    REQUIRE(clock.SampleCount() == 100);
    REQUIRE(clock.Percentile(50.0) == Catch::Approx(50.0));
    REQUIRE(clock.Percentile(99.0) == Catch::Approx(99.0));
    REQUIRE(clock.Percentile(100.0) == Catch::Approx(100.0));
    REQUIRE(clock.Percentile(0.0) == Catch::Approx(1.0));
}

TEST_CASE("frame clock: ring buffer keeps only the newest samples", "[perf]") {
    FrameClock clock(10);
    for (int i = 1; i <= 25; ++i) clock.AddSample(static_cast<double>(i));

    REQUIRE(clock.SampleCount() == 10);
    REQUIRE(clock.LastMs() == Catch::Approx(25.0));
    // Samples 16..25 remain, so the average is 20.5.
    REQUIRE(clock.AverageMs() == Catch::Approx(20.5));
}

TEST_CASE("frame clock: empty clock reports zeros instead of dividing by zero", "[perf]") {
    FrameClock clock;
    REQUIRE(clock.SampleCount() == 0);
    REQUIRE(clock.AverageMs() == Catch::Approx(0.0));
    REQUIRE(clock.Percentile(99.0) == Catch::Approx(0.0));
    REQUIRE(clock.AverageFps() == Catch::Approx(0.0));
}

TEST_CASE("frame clock: Tick measures real elapsed time", "[perf]") {
    FrameClock clock;
    REQUIRE(clock.Tick() == Catch::Approx(0.0));  // baseline only
    ::Sleep(20);
    const double elapsed = clock.Tick();
    REQUIRE(elapsed > 10.0);
    REQUIRE(elapsed < 200.0);
    REQUIRE(clock.SampleCount() == 1);
}

TEST_CASE("frame clock: 60 FPS average converts back to 60 FPS", "[perf]") {
    FrameClock clock;
    for (int i = 0; i < 60; ++i) clock.AddSample(1000.0 / 60.0);
    REQUIRE(clock.AverageFps() == Catch::Approx(60.0).epsilon(0.01));
}

TEST_CASE("app loop: target interval is clamped to a sane range", "[loop]") {
    AppLoop loop;
    loop.SetTargetIntervalMs(0);
    REQUIRE(loop.TargetIntervalMs() == 1);
    loop.SetTargetIntervalMs(16);
    REQUIRE(loop.TargetIntervalMs() == 16);
    loop.SetTargetIntervalMs(99999);
    REQUIRE(loop.TargetIntervalMs() == 1000);
}
