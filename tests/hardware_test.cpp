// DockForge - T-068 hardware panel unit tests (Catch2).
//
// What is testable without a window (everything HWND-bound is covered by the
// machine probe instead):
//   - CPU% math over injected counter snapshots (idle-inside-kernel rule,
//     degenerate deltas, backwards clocks)
//   - memory percent + level formatting
//   - sidecar line protocol parsing (poll answer / push / failure / garbage)
//   - C-23 event mapping from sidecar snapshots (RadioStateChanged,
//     MediaSessionChanged)
//   - media clock formatting
//   - session display name shaping (pure string part)
#include <catch_amalgamated.hpp>

#include "finderbar/audio_hardware.h"
#include "finderbar/hardware_types.h"
#include "finderbar/media_sidecar.h"
#include "finderbar/system_stats.h"

#include <windows.h>

namespace {

using dockforge::finderbar::ComputeCpuPercent;
using dockforge::finderbar::ComputeMemoryPercent;
using dockforge::finderbar::FindRadio;
using dockforge::finderbar::FormatLevelPercent;
using dockforge::finderbar::FormatMediaClock;
using dockforge::finderbar::MediaEventFrom;
using dockforge::finderbar::MediaSessionChanged;
using dockforge::finderbar::ParseSidecarLine;
using dockforge::finderbar::RadioEventFrom;
using dockforge::finderbar::SidecarRadioState;
using dockforge::finderbar::SidecarState;
using dockforge::finderbar::SystemCounters;
using dockforge::finderbar::SystemStatsSampler;

SystemCounters Counters(uint64_t idle, uint64_t kernel, uint64_t user,
                        uint64_t total_gb = 0, uint64_t used_gb = 0) {
    SystemCounters c;
    c.idle_ticks = idle;
    c.kernel_ticks = kernel;
    c.user_ticks = user;
    const uint64_t b = 1024ULL * 1024ULL * 1024ULL;
    c.memory_total_bytes = total_gb * b;
    c.memory_used_bytes = used_gb * b;
    return c;
}

}  // namespace

TEST_CASE("CPU percent math: idle is inside kernel", "[finderbar]") {
    // Total elapsed CPU = (kernel2-kernel1) + (user2-user1) = 100 + 100 = 200.
    // Idle delta 150 of those 200 -> busy 25%.
    const SystemCounters a = Counters(1000, 2000, 500);
    const SystemCounters b = Counters(1150, 2100, 600);
    REQUIRE(ComputeCpuPercent(a, b) == Catch::Approx(25.0f).epsilon(0.01f));
}

TEST_CASE("CPU percent math: fully idle reads zero", "[finderbar]") {
    // Everything that elapsed is idle: busy = 0.
    const SystemCounters a = Counters(100, 200, 50);
    const SystemCounters b = Counters(300, 400, 50);
    REQUIRE(ComputeCpuPercent(a, b) == Catch::Approx(0.0f).margin(0.001f));
}

TEST_CASE("CPU percent math: fully busy reads 100", "[finderbar]") {
    const SystemCounters a = Counters(100, 200, 50);
    const SystemCounters b = Counters(100, 300, 150);
    REQUIRE(ComputeCpuPercent(a, b) == Catch::Approx(100.0f).epsilon(0.01f));
}

TEST_CASE("CPU percent math: degenerate and backwards deltas read zero",
          "[finderbar]") {
    REQUIRE(ComputeCpuPercent(Counters(1, 1, 1), Counters(1, 1, 1)) == 0.0f);
    // After sleep the counters can run backwards; must not produce garbage.
    REQUIRE(ComputeCpuPercent(Counters(500, 800, 100), Counters(100, 200, 50)) == 0.0f);
}

TEST_CASE("memory percent clamps and formats", "[finderbar]") {
    REQUIRE(ComputeMemoryPercent(5, 10) == Catch::Approx(50.0f).epsilon(0.01f));
    REQUIRE(ComputeMemoryPercent(0, 0) == 0.0f);
    REQUIRE(ComputeMemoryPercent(20, 10) == 100.0f);
    REQUIRE(FormatLevelPercent(0.765f) == L"77%");
    REQUIRE(FormatLevelPercent(-0.5f) == L"0%");
    REQUIRE(FormatLevelPercent(1.5f) == L"100%");
    REQUIRE(FormatLevelPercent(0.0f) == L"0%");
    REQUIRE(FormatLevelPercent(1.0f) == L"100%");
}

TEST_CASE("sampler: first update reports zero CPU, second uses the delta",
          "[finderbar]") {
    static SystemCounters fake;
    fake = Counters(1000, 2000, 500);
    int calls = 0;
    static int* calls_ptr = &calls;
    SystemStatsSampler sampler;
    sampler.SetCounterSourceForTest([]() {
        ++(*calls_ptr);
        return fake;
    });
    const auto first = sampler.Update();
    REQUIRE(first.cpu_percent == 0.0f);
    fake = Counters(1150, 2100, 600);
    const auto second = sampler.Update();
    REQUIRE(second.cpu_percent == Catch::Approx(25.0f).epsilon(0.01f));
    REQUIRE(second.memory_total_bytes == 0);
    sampler.SetCounterSourceForTest(nullptr);
}

TEST_CASE("sidecar parse: full poll answer", "[finderbar]") {
    const std::string line =
        "{\"ok\":true,\"media\":{\"aumid\":\"Chrome\",\"title\":\"T\",\"artist\":\"A\","
        "\"playing\":true,\"has_session\":true,\"pos\":100,\"dur\":200},"
        "\"radios\":[{\"name\":\"蓝牙\",\"kind\":3,\"state\":1},"
        "{\"name\":\"WLAN\",\"kind\":1,\"state\":2}]}";
    SidecarState state;
    bool is_push = true;
    std::string push_kind = "x";
    std::wstring error = L"x";
    REQUIRE(ParseSidecarLine(line, &state, &is_push, &push_kind, &error));
    REQUIRE(!is_push);
    REQUIRE(state.valid);
    REQUIRE(state.media.has_session);
    REQUIRE(state.media.aumid == L"Chrome");
    REQUIRE(state.media.title == L"T");
    REQUIRE(state.media.artist == L"A");
    REQUIRE(state.media.playing);
    REQUIRE(state.media.position_ticks == 100);
    REQUIRE(state.media.duration_ticks == 200);
    REQUIRE(state.radios.size() == 2);
    REQUIRE(state.radios[0].kind == 3);
    REQUIRE(state.radios[0].name == L"蓝牙");  // UTF-8 round trip
    REQUIRE(state.radios[0].state == 1);
    REQUIRE(state.radios[1].kind == 1);
    REQUIRE(state.radios[1].state == 2);
}

TEST_CASE("sidecar parse: push line, failure line, garbage line", "[finderbar]") {
    SidecarState state;
    bool is_push = false;
    std::string push_kind;
    std::wstring error;
    REQUIRE(ParseSidecarLine("{\"push\":\"media\"}", &state, &is_push, &push_kind,
                             &error));
    REQUIRE(is_push);
    REQUIRE(push_kind == "media");
    REQUIRE(!state.valid);

    REQUIRE(ParseSidecarLine("{\"ok\":false,\"error\":\"no media session\"}", &state,
                             &is_push, &push_kind, &error));
    REQUIRE(!is_push);
    REQUIRE(error == L"no media session");

    REQUIRE(!ParseSidecarLine("not json at all", &state, &is_push, &push_kind, &error));
    REQUIRE(!ParseSidecarLine("{\"unexpected\":\"shape\"}", &state, &is_push,
                              &push_kind, &error));
}

TEST_CASE("sidecar parse: ready banner and plain ack are valid but carry no state",
          "[finderbar]") {
    SidecarState state;
    bool is_push = false;
    std::string push_kind;
    std::wstring error;
    REQUIRE(ParseSidecarLine("{\"ok\":true,\"sidecar\":\"ready\",\"pid\":123}", &state,
                             &is_push, &push_kind, &error));
    REQUIRE(!state.valid);
    REQUIRE(ParseSidecarLine("{\"ok\":true}", &state, &is_push, &push_kind, &error));
    REQUIRE(!state.valid);
}

TEST_CASE("radio lookup + C-23 event mapping", "[finderbar]") {
    SidecarState state;
    SidecarRadioState wifi;
    wifi.kind = 1;
    wifi.state = 1;  // On
    SidecarRadioState bt;
    bt.kind = 3;
    bt.state = 3;  // Disabled
    state.radios = {wifi, bt};
    REQUIRE(FindRadio(state, 1) != nullptr);
    REQUIRE(FindRadio(state, 3) != nullptr);
    REQUIRE(FindRadio(state, 2) == nullptr);

    const auto wifi_event = RadioEventFrom(wifi);
    REQUIRE(wifi_event.radio_kind == 1);
    REQUIRE(wifi_event.on);
    const auto bt_event = RadioEventFrom(bt);
    REQUIRE(bt_event.radio_kind == 3);
    REQUIRE(!bt_event.on);  // Disabled is not On
}

TEST_CASE("media event mapping distinguishes no-session from paused",
          "[finderbar]") {
    dockforge::finderbar::SidecarMediaState paused;
    paused.has_session = true;
    paused.aumid = L"Chrome";
    paused.title = L"song";
    paused.playing = false;
    const auto e1 = MediaEventFrom(paused);
    REQUIRE(e1.has_session);
    REQUIRE(e1.session_id == L"Chrome");
    REQUIRE(!e1.playing);

    dockforge::finderbar::SidecarMediaState none;
    const auto e2 = MediaEventFrom(none);
    REQUIRE(!e2.has_session);
}

TEST_CASE("media clock formatting", "[finderbar]") {
    REQUIRE(FormatMediaClock(0) == L"--:--");
    REQUIRE(FormatMediaClock(-5) == L"--:--");
    // 1 second
    REQUIRE(FormatMediaClock(10000000LL) == L"0:01");
    // 61 seconds
    REQUIRE(FormatMediaClock(61LL * 10000000LL) == L"1:01");
    // 1 hour 1 minute 1 second
    REQUIRE(FormatMediaClock(3661LL * 10000000LL) == L"1:01:01");
}

TEST_CASE("session display name shaping (pure part)", "[finderbar]") {
    // pid 0 is the system session the audiosrv owns - always the fixed label.
    REQUIRE(dockforge::finderbar::SessionDisplayName(0) == L"系统声音");
    // A pid that cannot be opened degrades to a numbered label, never throws.
    const std::wstring dead = dockforge::finderbar::SessionDisplayName(0x7FFFFFFF);
    REQUIRE(!dead.empty());
}

TEST_CASE("control error shape (C-23: no silent failures)", "[finderbar]") {
    const auto ok = dockforge::finderbar::HardwareControlError::None();
    REQUIRE(ok.ok());
    const auto bad = dockforge::finderbar::HardwareControlError::Failed(
        L"主音量", L"设置音量失败 (0x80004005)", 0x80004005);
    REQUIRE(!bad.ok());
    REQUIRE(bad.hresult == 0x80004005);
    REQUIRE(bad.scope == L"主音量");
}
