// DockForge - T-076 notification centre unit tests (Catch2).
//
// What is testable without a desktop (the windows themselves are covered by the
// probe channel in tools/notifications_probe.ps1):
//   - the sidecar line protocol, against the exact shapes measured in
//     evidence/T-076/spike_sidecar_protocol.log
//   - the text fallback chain (app -> aumid -> placeholder, title -> body line)
//   - the store's baseline rule and its "cleared stays cleared" rule
//   - the banner model: stacking, per-card expiry, the 4-card ceiling, hit-testing
//   - the panel layout: row window, scroll clamping, click -> row mapping
//   - AUMID/display-name tile matching
//   - C-21's MaterialMonitor ladder and restore
//   - the notifications section of C-01
#include "catch_amalgamated.hpp"

#include "core/config.h"
#include "core/event_bus.h"
#include "notifications/notification_banner.h"
#include "notifications/notification_panel.h"
#include "notifications/notification_sidecar.h"
#include "notifications/notification_store.h"
#include "notifications/notification_targets.h"
#include "render/material_events.h"
#include "render/material_monitor.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace dockforge;
using notifications::AccessStatus;
using notifications::BannerAlphaForTier;
using notifications::BannerModel;
using notifications::FindTileForNotification;
using notifications::FoldNotificationLines;
using notifications::kMaxBannerCards;
using notifications::kMaxHistoryItems;
using notifications::kNoTile;
using notifications::NotificationDrain;
using notifications::NotificationItem;
using notifications::NotificationState;
using notifications::NotificationStore;
using notifications::PanelHit;
using notifications::PanelLayout;
using notifications::ParseNotificationLine;
using notifications::TileIdentity;

namespace {

// Verbatim shapes from evidence/T-076/spike_sidecar_protocol.log.
const char* kReadyLine =
    R"({"ok":true,"sidecar":"ready","pid":4242,"access":1})";

const char* kPollLine =
    R"({"ok":true,"access":1,"count":3,"items":[)"
    R"({"id":13967,"aumid":"Windows.Defender.SecurityCenter","app":"Windows 安全中心",)"
    R"("title":"启用病毒防护","body":"","created":1792940050},)"
    R"({"id":13962,"aumid":"","app":"","title":"任务完成","body":"网络层已打通","created":1792940000},)"
    R"({"id":13960,"aumid":"com.qoder.app","app":"Qoder","title":"","body":"构建成功","created":1792939000}]})";

const char* kEmptyPollLine = R"({"ok":true,"access":1,"count":0,"items":[]})";
const char* kAckLine = R"({"ok":true})";
const char* kFailLine = R"({"ok":false,"error":"bad command"})";
const char* kPushLine = R"({"push":"notifications"})";

NotificationItem MakeItem(std::uint32_t id, const wchar_t* app, const wchar_t* title,
                          const wchar_t* body) {
    NotificationItem item;
    item.id = id;
    item.app = app;
    item.title = title;
    item.body = body;
    return item;
}

NotificationState MakeState(std::vector<NotificationItem> items, bool valid = true) {
    NotificationState state;
    state.valid = valid;
    state.access = AccessStatus::Allowed;
    state.total = static_cast<int>(items.size());
    state.items = std::move(items);
    return state;
}

}  // namespace

// ---------------------------------------------------------------- protocol
TEST_CASE("notification protocol: ready line carries access but no list", "[notifications]") {
    NotificationState state;
    bool push = false;
    bool ready = false;
    std::wstring error;
    REQUIRE(ParseNotificationLine(kReadyLine, &state, &push, &ready, &error));
    CHECK(ready);
    CHECK_FALSE(push);
    CHECK(state.access == AccessStatus::Allowed);
    CHECK_FALSE(state.valid);  // a ready banner is not a poll answer
    CHECK(state.items.empty());
    CHECK(error.empty());
}

TEST_CASE("notification protocol: poll answer fills items in wire order", "[notifications]") {
    NotificationState state;
    bool push = false;
    bool ready = false;
    REQUIRE(ParseNotificationLine(kPollLine, &state, &push, &ready, nullptr));
    CHECK_FALSE(ready);
    CHECK(state.valid);
    CHECK(state.access == AccessStatus::Allowed);
    CHECK(state.total == 3);
    REQUIRE(state.items.size() == 3);
    // Newest first, exactly as the sidecar sends it.
    CHECK(state.items[0].id == 13967u);
    CHECK(state.items[0].app == L"Windows 安全中心");
    CHECK(state.items[1].id == 13962u);
    CHECK(state.items[1].aumid.empty());  // Win32 toasts carry no AUMID
    CHECK(state.items[2].id == 13960u);
    CHECK(state.items[2].created_unix == 1792939000);
}

TEST_CASE("notification protocol: push, bare ack, failure and garbage", "[notifications]") {
    NotificationState state;
    bool push = false;
    bool ready = false;
    std::wstring error;

    REQUIRE(ParseNotificationLine(kPushLine, &state, &push, &ready, &error));
    CHECK(push);
    CHECK_FALSE(state.valid);

    push = false;
    REQUIRE(ParseNotificationLine(kAckLine, &state, &push, &ready, &error));
    CHECK_FALSE(push);
    CHECK_FALSE(state.valid);  // an ack says nothing about the list

    REQUIRE(ParseNotificationLine(kFailLine, &state, &push, &ready, &error));
    CHECK(error == L"bad command");
    CHECK_FALSE(state.valid);

    // Garbage is rejected outright rather than treated as an empty poll - an
    // empty poll would silently wipe the panel.
    CHECK_FALSE(ParseNotificationLine("not json at all", &state, &push, &ready, &error));
    CHECK_FALSE(ParseNotificationLine(R"([1,2,3])", &state, &push, &ready, &error));
    CHECK_FALSE(ParseNotificationLine(R"({"ok":"yes"})", &state, &push, &ready, &error));
}

TEST_CASE("notification protocol: empty poll is a valid, empty answer", "[notifications]") {
    NotificationState state;
    REQUIRE(ParseNotificationLine(kEmptyPollLine, &state, nullptr, nullptr, nullptr));
    CHECK(state.valid);
    CHECK(state.total == 0);
    CHECK(state.items.empty());
}

TEST_CASE("notification protocol: folding keeps every poll answer in order", "[notifications]") {
    NotificationDrain drain;
    FoldNotificationLines({kReadyLine, kPollLine, kPushLine, kEmptyPollLine, "junk"}, &drain);
    CHECK(drain.ready);
    CHECK(drain.ready_access == AccessStatus::Allowed);
    CHECK(drain.pushed);
    CHECK(drain.lines == 4);  // junk is dropped, not counted
    REQUIRE(drain.states.size() == 2);
    CHECK(drain.states[0].items.size() == 3);
    CHECK(drain.states[1].items.empty());
}

// ---------------------------------------------------------------- text
TEST_CASE("notification text: display app falls back to aumid then a placeholder",
          "[notifications]") {
    CHECK(MakeItem(1, L"Outlook", L"t", L"").DisplayApp() == L"Outlook");
    NotificationItem aumid_only = MakeItem(2, L"", L"t", L"");
    aumid_only.aumid = L"com.qoder.app";
    CHECK(aumid_only.DisplayApp() == L"com.qoder.app");
    CHECK(MakeItem(3, L"", L"", L"").DisplayApp() == L"通知");
}

TEST_CASE("notification text: headline prefers title, then the body's first line",
          "[notifications]") {
    CHECK(MakeItem(1, L"a", L"标题", L"正文").Headline() == L"标题");
    // No title: the first line becomes the headline so the banner still says
    // something, and the rest stays available as the detail.
    const NotificationItem two_lines = MakeItem(2, L"a", L"", L"第一行\n第二行");
    CHECK(two_lines.Headline() == L"第一行");
    CHECK(two_lines.Detail() == L"第二行");
    CHECK(MakeItem(3, L"a", L"", L"").Headline() == L"a");
    // With a title, the whole body is the detail.
    CHECK(MakeItem(4, L"a", L"标题", L"正文").Detail() == L"正文");
    // Nothing left after the headline is empty, not a repeat.
    CHECK(MakeItem(5, L"a", L"", L"只有一行").Detail().empty());
}

// ---------------------------------------------------------------- store
TEST_CASE("notification store: the first poll is a baseline, not an arrival",
          "[notifications]") {
    NotificationStore store;
    CHECK_FALSE(store.primed());
    std::vector<NotificationItem> arrived;
    // Windows keeps a backlog; treating it as news would greet every launch
    // with 60+ banners.
    const bool changed = store.ApplyState(
        MakeState({MakeItem(3, L"c", L"t3", L""), MakeItem(2, L"b", L"t2", L""),
                   MakeItem(1, L"a", L"t1", L"")}),
        &arrived);
    CHECK(changed);
    CHECK(store.primed());
    CHECK(arrived.empty());
    CHECK(store.size() == 3);
    CHECK(store.total_reported() == 3);
}

TEST_CASE("notification store: later polls report only unseen ids, oldest first",
          "[notifications]") {
    NotificationStore store;
    store.ApplyState(MakeState({MakeItem(1, L"a", L"t1", L"")}), nullptr);

    std::vector<NotificationItem> arrived;
    // Two new toasts on top of the known one.
    const bool changed = store.ApplyState(
        MakeState({MakeItem(3, L"c", L"t3", L""), MakeItem(2, L"b", L"t2", L""),
                   MakeItem(1, L"a", L"t1", L"")}),
        &arrived);
    CHECK(changed);
    REQUIRE(arrived.size() == 2);
    CHECK(arrived[0].id == 2u);  // oldest first: banners stack in the order raised
    CHECK(arrived[1].id == 3u);
}

TEST_CASE("notification store: an unchanged list reports no arrival", "[notifications]") {
    NotificationStore store;
    store.ApplyState(MakeState({MakeItem(1, L"a", L"t1", L"")}), nullptr);
    std::vector<NotificationItem> arrived;
    const bool changed = store.ApplyState(MakeState({MakeItem(1, L"a", L"t1", L"")}), &arrived);
    CHECK_FALSE(changed);
    CHECK(arrived.empty());
}

TEST_CASE("notification store: a cleared history stays cleared", "[notifications]") {
    NotificationStore store;
    store.ApplyState(MakeState({MakeItem(1, L"a", L"t1", L""), MakeItem(2, L"b", L"t2", L"")}),
                     nullptr);
    store.ClearLocal();
    CHECK(store.empty());

    // Windows still holds both toasts, so the next poll returns them again. They
    // must not come back as arrivals - the user dismissed them on purpose.
    std::vector<NotificationItem> arrived;
    store.ApplyState(MakeState({MakeItem(2, L"b", L"t2", L""), MakeItem(1, L"a", L"t1", L"")}),
                     &arrived);
    CHECK(arrived.empty());
    // The list itself is rebuilt from the poll (the panel shows what Windows
    // holds), but nothing is announced.
    CHECK(store.size() == 2);
}

TEST_CASE("notification store: an invalid state changes nothing", "[notifications]") {
    NotificationStore store;
    store.ApplyState(MakeState({MakeItem(1, L"a", L"t1", L"")}), nullptr);
    std::vector<NotificationItem> arrived;
    CHECK_FALSE(store.ApplyState(MakeState({}, false), &arrived));
    CHECK(store.size() == 1);
    CHECK(arrived.empty());
}

TEST_CASE("notification store: the history is capped", "[notifications]") {
    NotificationStore store;
    std::vector<NotificationItem> many;
    for (std::uint32_t i = 0; i < kMaxHistoryItems + 15; ++i) {
        many.push_back(MakeItem(i + 1, L"a", L"t", L""));
    }
    store.ApplyState(MakeState(many), nullptr);
    CHECK(store.size() == kMaxHistoryItems);
    CHECK(store.IndexOf(1) == 0);
    CHECK(store.IndexOf(static_cast<std::uint32_t>(kMaxHistoryItems + 15)) == -1);
}

// ---------------------------------------------------------------- banner
TEST_CASE("banner: newest card on top, stack capped", "[notifications]") {
    BannerModel model;
    CHECK(model.empty());
    CHECK(model.HeightDip() == 0.0);
    CHECK(model.Push(MakeItem(1, L"a", L"first", L"")));
    CHECK(model.Push(MakeItem(2, L"a", L"second", L"")));
    REQUIRE(model.size() == 2);
    CHECK(model.cards()[0].item.id == 2u);  // newest first

    for (std::uint32_t i = 3; i <= 8; ++i) {
        model.Push(MakeItem(i, L"a", L"t", L""));
    }
    CHECK(model.size() == kMaxBannerCards);
    CHECK(model.cards()[0].item.id == 8u);
    // The oldest cards were the ones dropped.
    CHECK(model.cards()[kMaxBannerCards - 1].item.id == 5u);
}

TEST_CASE("banner: an id already on screen is not pushed twice", "[notifications]") {
    BannerModel model;
    CHECK(model.Push(MakeItem(7, L"a", L"t", L"")));
    // The sidecar re-announces a change with the full list, so the same id can
    // arrive twice in one poll window.
    CHECK_FALSE(model.Push(MakeItem(7, L"a", L"t", L"")));
    CHECK(model.size() == 1);
    // id 0 is never a real notification.
    CHECK_FALSE(model.Push(MakeItem(0, L"a", L"t", L"")));
}

TEST_CASE("banner: each card expires on its own clock", "[notifications]") {
    BannerModel model;
    model.SetTimeoutSeconds(5);
    CHECK(model.timeout_seconds() == 5);
    model.Push(MakeItem(1, L"a", L"old", L""));
    // 4s later a second card arrives; the first one is nearly done.
    CHECK_FALSE(model.Tick(4000.0));
    model.Push(MakeItem(2, L"a", L"new", L""));
    CHECK(model.size() == 2);
    // The first card hits its 5s and goes; the second must NOT be kept alive by
    // the tick that removed the first.
    CHECK(model.Tick(1000.0));
    REQUIRE(model.size() == 1);
    CHECK(model.cards()[0].item.id == 2u);
    // A tick with nothing expiring reports no change: the window must not
    // repaint its idle frames.
    CHECK_FALSE(model.Tick(1000.0));
}

TEST_CASE("banner: dismiss and clear", "[notifications]") {
    BannerModel model;
    model.Push(MakeItem(1, L"a", L"t1", L""));
    model.Push(MakeItem(2, L"a", L"t2", L""));
    CHECK(model.Dismiss(1));
    CHECK(model.size() == 1);
    CHECK(model.cards()[0].item.id == 2u);
    CHECK_FALSE(model.Dismiss(5));  // out of range is a no-op
    model.Clear();
    CHECK(model.empty());
}

TEST_CASE("banner: hit-testing maps a y offset to a card and skips the gaps",
          "[notifications]") {
    BannerModel model;
    model.Push(MakeItem(1, L"a", L"t1", L""));
    model.Push(MakeItem(2, L"a", L"t2", L""));
    REQUIRE(model.size() == 2);
    // Card 0 occupies [0, 78), the 8 DIP gap is [78, 86), card 1 starts at 86.
    CHECK(model.CardAtDip(0.0) == 0);
    CHECK(model.CardAtDip(77.9) == 0);
    CHECK(model.CardAtDip(80.0) == BannerModel::kNoCard);  // the gap belongs to no card
    CHECK(model.CardAtDip(86.0) == 1);
    CHECK(model.CardAtDip(1000.0) == BannerModel::kNoCard);
    CHECK(model.CardAtDip(-5.0) == BannerModel::kNoCard);
}

TEST_CASE("banner: height follows the stack", "[notifications]") {
    BannerModel model;
    model.Push(MakeItem(1, L"a", L"t", L""));
    const double one = model.HeightDip();
    model.Push(MakeItem(2, L"a", L"t", L""));
    const double two = model.HeightDip();
    // One card + one gap + one card.
    CHECK(two > one);
    CHECK(two - one == Catch::Approx(notifications::kBannerCardHeightDip +
                                     notifications::kBannerGapDip));
}

TEST_CASE("banner: a degraded material is the more opaque one", "[notifications]") {
    // C-21's whole point: degradation must cost less GPU, so the ladder goes
    // toward opaque. A regression that inverted it would make battery saver
    // more expensive than the normal path.
    const BYTE mica = BannerAlphaForTier(core::MaterialLevel::Mica);
    const BYTE acrylic = BannerAlphaForTier(core::MaterialLevel::Acrylic);
    const BYTE blur = BannerAlphaForTier(core::MaterialLevel::Blur);
    const BYTE solid = BannerAlphaForTier(core::MaterialLevel::Solid);
    CHECK(mica < acrylic);
    CHECK(acrylic < blur);
    CHECK(blur < solid);
    CHECK(solid == 255);
}

// ---------------------------------------------------------------- panel
TEST_CASE("panel layout: rows, height and scroll clamping", "[notifications]") {
    const PanelLayout empty = PanelLayout::Compute(0, 0);
    CHECK(empty.visible_rows == 0);
    CHECK(empty.first_row == 0);
    CHECK(empty.height_dip ==
          Catch::Approx(notifications::kPanelHeaderHeightDip +
                        notifications::kPanelFooterHeightDip));

    const PanelLayout few = PanelLayout::Compute(3, 0);
    CHECK(few.visible_rows == 3);
    CHECK(few.height_dip ==
          Catch::Approx(notifications::kPanelHeaderHeightDip +
                        notifications::kPanelFooterHeightDip +
                        3 * notifications::kPanelRowHeightDip));

    // Past the ceiling the list scrolls instead of growing.
    const PanelLayout many = PanelLayout::Compute(30, 0);
    CHECK(many.visible_rows ==
          static_cast<std::size_t>(notifications::kPanelMaxRowsDip));

    // A scroll past the tail is clamped: the panel must never render empty.
    const PanelLayout clamped = PanelLayout::Compute(30, 1000);
    CHECK(clamped.first_row == 30 - clamped.visible_rows);
}

TEST_CASE("panel hit-testing: rows, the clear button and empty space", "[notifications]") {
    const PanelLayout layout = PanelLayout::Compute(5, 0);
    const double width = notifications::kPanelWidthDip;

    // Header is inert.
    const PanelHit header = layout.HitTest(10.0, width * 0.5, width);
    CHECK(header.kind == PanelHit::Kind::None);

    // Row 0 starts right below the header.
    const PanelHit row0 =
        layout.HitTest(notifications::kPanelHeaderHeightDip + 1.0, width * 0.5, width);
    CHECK(row0.kind == PanelHit::Kind::Row);
    CHECK(row0.index == 0);

    const PanelHit row2 = layout.HitTest(
        notifications::kPanelHeaderHeightDip + 2 * notifications::kPanelRowHeightDip + 4.0,
        width * 0.5, width);
    CHECK(row2.kind == PanelHit::Kind::Row);
    CHECK(row2.index == 2);

    // The footer's right-hand side is the clear button.
    const double footer_y = layout.height_dip - 6.0;
    const PanelHit clear = layout.HitTest(footer_y, width - 10.0, width);
    CHECK(clear.kind == PanelHit::Kind::Clear);
    const PanelHit footer_left = layout.HitTest(footer_y, 10.0, width);
    CHECK(footer_left.kind == PanelHit::Kind::None);
}

TEST_CASE("panel hit-testing: a scrolled list reports absolute row indices",
          "[notifications]") {
    const PanelLayout layout = PanelLayout::Compute(30, 10);
    REQUIRE(layout.first_row == 10);
    const PanelHit first =
        layout.HitTest(notifications::kPanelHeaderHeightDip + 1.0,
                       notifications::kPanelWidthDip * 0.5, notifications::kPanelWidthDip);
    CHECK(first.kind == PanelHit::Kind::Row);
    CHECK(first.index == 10);  // not 0: the click must select the row it shows
}

// ---------------------------------------------------------------- targets
TEST_CASE("notification targets: exact AUMID wins over a display name", "[notifications]") {
    std::vector<TileIdentity> tiles = {
        {L"com.qoder.app", L"Qoder"},
        {L"Windows.Defender.SecurityCenter", L"Windows 安全中心"},
    };
    NotificationItem item = MakeItem(1, L"Windows 安全中心", L"t", L"");
    item.aumid = L"Windows.Defender.SecurityCenter";
    CHECK(FindTileForNotification(item, tiles) == 1);
}

TEST_CASE("notification targets: a package-prefix AUMID still matches", "[notifications]") {
    // The toast carries the app suffix, the shortcut often does not (or the
    // other way round).
    std::vector<TileIdentity> tiles = {
        {L"Microsoft.OutlookForWindows_8wekyb3d8bbwe", L"Outlook"},
    };
    NotificationItem item = MakeItem(1, L"Outlook", L"t", L"");
    item.aumid = L"Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows";
    CHECK(FindTileForNotification(item, tiles) == 0);
}

TEST_CASE("notification targets: a nameless Win32 toast matches by display name",
          "[notifications]") {
    std::vector<TileIdentity> tiles = {
        {L"", L"Microsoft Outlook"},
        {L"com.qoder.app", L"Qoder"},
    };
    // No AUMID at all (four of 63 measured toasts), and the toast's name is a
    // fragment of the pinned one.
    NotificationItem item = MakeItem(1, L"Outlook", L"t", L"");
    CHECK(FindTileForNotification(item, tiles) == 0);
    // Case only differs.
    NotificationItem upper = MakeItem(2, L"QODER", L"t", L"");
    CHECK(FindTileForNotification(upper, tiles) == 1);
    // Nothing matches -> no tile, so nothing pulses.
    NotificationItem stranger = MakeItem(3, L"Some Random App", L"t", L"");
    CHECK(FindTileForNotification(stranger, tiles) == kNoTile);
    // An empty tile list is not a crash.
    CHECK(FindTileForNotification(stranger, std::vector<TileIdentity>{}) == kNoTile);
}

TEST_CASE("notification targets: ascii-only lowercasing", "[notifications]") {
    CHECK(notifications::LowerAsciiWide(L"QoDeR") == L"qoder");
    // Non-ASCII is left alone: a locale fold would make the same toast match
    // different tiles on different machines.
    CHECK(notifications::LowerAsciiWide(L"安全中心") == L"安全中心");
    CHECK(notifications::LowerAsciiWide(L"").empty());
}

// ---------------------------------------------------------------- C-21
namespace {

struct PowerScript {
    std::vector<render::PowerSnapshot> steps;
    std::size_t index = 0;
    render::PowerSnapshot Next() {
        if (steps.empty()) return {};
        const std::size_t at = std::min(index, steps.size() - 1);
        ++index;
        return steps[at];
    }
};

}  // namespace

TEST_CASE("material monitor: battery saver steps down one tier per poll",
          "[notifications]") {
    core::EventBus bus;
    PowerScript script;
    script.steps = {{false, false}, {true, true}, {true, true}, {true, true}, {true, true}};
    render::MaterialMonitor monitor(bus, [&] { return script.Next(); }, 5000.0);
    monitor.SetBaseline(render::MaterialBaseline{core::MaterialLevel::Mica,
                                                 core::MaterialLevel::Mica});

    std::vector<core::MaterialLevel> degraded_to;
    bus.Subscribe<core::MaterialDegraded>(
        [&](const core::MaterialDegraded& e) { degraded_to.push_back(e.to); });

    CHECK(monitor.current_tier() == core::MaterialLevel::Mica);
    // First tick polls immediately (since_poll starts at a full interval).
    monitor.Tick(0.0);
    CHECK_FALSE(monitor.degraded());

    monitor.Tick(5000.0);
    CHECK(monitor.degraded());
    CHECK(monitor.current_tier() == core::MaterialLevel::Acrylic);
    monitor.Tick(5000.0);
    CHECK(monitor.current_tier() == core::MaterialLevel::Blur);
    monitor.Tick(5000.0);
    CHECK(monitor.current_tier() == core::MaterialLevel::Solid);
    // Already at the bottom: no further events, so the log does not repeat.
    monitor.Tick(5000.0);
    REQUIRE(degraded_to.size() == 3);
    CHECK(degraded_to[0] == core::MaterialLevel::Acrylic);
    CHECK(degraded_to[1] == core::MaterialLevel::Blur);
    CHECK(degraded_to[2] == core::MaterialLevel::Solid);
}

TEST_CASE("material monitor: restore jumps straight back to the resolved tier",
          "[notifications]") {
    core::EventBus bus;
    PowerScript script;
    script.steps = {{false, false}, {true, true}, {true, true}, {false, false}};
    render::MaterialMonitor monitor(bus, [&] { return script.Next(); }, 5000.0);
    // The machine can only do Acrylic, so that is what it restores to - not the
    // requested Mica.
    monitor.SetBaseline(
        render::MaterialBaseline{core::MaterialLevel::Mica, core::MaterialLevel::Acrylic});

    std::vector<core::MaterialLevel> restored_to;
    bus.Subscribe<core::MaterialRestored>(
        [&](const core::MaterialRestored& e) { restored_to.push_back(e.to); });

    monitor.Tick(0.0);
    monitor.Tick(5000.0);  // saver on -> Blur (one below Acrylic)
    CHECK(monitor.degraded());
    CHECK(monitor.current_tier() == core::MaterialLevel::Blur);
    monitor.Tick(5000.0);  // saver still on -> Solid
    CHECK(monitor.current_tier() == core::MaterialLevel::Solid);
    monitor.Tick(5000.0);  // saver off -> back in one step
    CHECK_FALSE(monitor.degraded());
    CHECK(monitor.current_tier() == core::MaterialLevel::Acrylic);
    REQUIRE(restored_to.size() == 1);
    CHECK(restored_to[0] == core::MaterialLevel::Acrylic);
}

TEST_CASE("material monitor: a lost device goes straight to solid", "[notifications]") {
    core::EventBus bus;
    render::MaterialMonitor monitor(bus, [] { return render::PowerSnapshot{}; }, 5000.0);
    monitor.SetBaseline(render::MaterialBaseline{core::MaterialLevel::Mica,
                                                 core::MaterialLevel::Mica});
    std::vector<core::MaterialLevel> degraded_to;
    bus.Subscribe<core::MaterialDegraded>(
        [&](const core::MaterialDegraded& e) { degraded_to.push_back(e.to); });

    monitor.Tick(0.0);
    // Skipping the ladder is deliberate: a lost device cannot composite at all,
    // so Solid is the only answer that renders.
    monitor.SetDeviceLost(true);
    CHECK(monitor.degraded());
    CHECK(monitor.current_tier() == core::MaterialLevel::Solid);
    REQUIRE(degraded_to.size() == 1);
    CHECK(degraded_to[0] == core::MaterialLevel::Solid);

    monitor.SetDeviceLost(false);
    CHECK_FALSE(monitor.degraded());
    CHECK(monitor.current_tier() == core::MaterialLevel::Mica);
}

// ---------------------------------------------------------------- config
TEST_CASE("config: the notifications section round-trips", "[notifications]") {
    core::Config config;
    // Defaults: off, banners on, pulse on, 5s, 2s.
    CHECK_FALSE(config.notifications.enabled);
    CHECK(config.notifications.banner_enabled);
    CHECK(config.notifications.pulse_icons);
    CHECK(config.notifications.banner_timeout_seconds == 5);
    CHECK(config.notifications.poll_seconds == 2);

    core::Config parsed;
    std::string error;
    REQUIRE(core::ParseConfig(
        R"({"notifications":{"enabled":true,"banner_enabled":false,"pulse_icons":false,)"
        R"("banner_timeout_seconds":9,"poll_seconds":7}})",
        parsed, &error));
    CHECK(parsed.notifications.enabled);
    CHECK_FALSE(parsed.notifications.banner_enabled);
    CHECK_FALSE(parsed.notifications.pulse_icons);
    CHECK(parsed.notifications.banner_timeout_seconds == 9);
    CHECK(parsed.notifications.poll_seconds == 7);

    // Serialise only writes the section when it differs from the default, so a
    // stock config.json stays clean.
    const std::string text = core::SerializeConfig(parsed);
    CHECK(text.find("\"notifications\"") != std::string::npos);
    core::Config round;
    REQUIRE(core::ParseConfig(text, round, &error));
    CHECK(core::SameNotifications(round.notifications, parsed.notifications));

    core::Config plain;
    CHECK(core::SerializeConfig(plain).find("\"notifications\"") == std::string::npos);
}

TEST_CASE("config: notification intervals are clamped to their ranges",
          "[notifications]") {
    CHECK(core::NotificationBannerTimeoutRange().min == 2);
    CHECK(core::NotificationBannerTimeoutRange().max == 30);
    CHECK(core::NotificationPollRange().min == 1);
    CHECK(core::NotificationPollRange().max == 30);

    core::Config parsed;
    std::string error;
    // A 0-second poll would spin against a sidecar that answers in ~700ms.
    REQUIRE(core::ParseConfig(
        R"({"notifications":{"poll_seconds":0,"banner_timeout_seconds":999}})", parsed,
        &error));
    CHECK(parsed.notifications.poll_seconds == 1);
    CHECK(parsed.notifications.banner_timeout_seconds == 30);
}
