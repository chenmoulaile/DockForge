// ---------------------------------------------------------------------------
// T-306: the right-region status indicators (finderbar.right ime/battery/network)
// - the signal-bars mapping, the battery colour tier, the flag decoding, the IME
// conversion-mode label and toggle, the connection-kind labels, and the layout
// path's wider battery icon box. The API readers (ImmGetDefaultIMEWnd /
// GetSystemPowerStatus / GetAdaptersAddresses / WlanQueryInterface) need a live
// machine and are covered by the evidence probes, not here.
// ---------------------------------------------------------------------------

#include "finderbar/status_icons.h"
#include "finderbar/status_sampler.h"

namespace {

using dockforge::finderbar::BatteryGradeForPercent;
using dockforge::finderbar::BatteryPresentForFlag;
using dockforge::finderbar::BatteryChargingFor;
using dockforge::finderbar::ConnectionKind;
using dockforge::finderbar::ConnectionKindLabel;
using dockforge::finderbar::ImeConversionLabel;
using dockforge::finderbar::ImeToggledConversionMode;
using dockforge::finderbar::RightItem;
using dockforge::finderbar::RightType;
using dockforge::finderbar::UsageGrade;
using dockforge::finderbar::WifiBarsForQuality;

// Builds one right entry with a text of a known fake width (FixedWidth above).
RightItem RItem(RightType type, int order, const std::wstring& text, bool has_icon,
                float icon_width = 16.0f) {
    RightItem item;
    item.type = type;
    item.order = order;
    item.text = text;
    item.has_icon = has_icon;
    item.icon_width_dip = icon_width;
    return item;
}

}  // namespace

TEST_CASE("T-306 wifi bars: wlanapi's 0-100 quality maps to four equal bands") {
    // Band edges: the ten-ths the mapping was pinned to (Beautiful
    // WifiSignal.Bars, Wifi.cs:26 - the same linear-percentage API).
    CHECK(WifiBarsForQuality(0) == 0);
    CHECK(WifiBarsForQuality(24) == 0);
    CHECK(WifiBarsForQuality(25) == 1);
    CHECK(WifiBarsForQuality(49) == 1);
    CHECK(WifiBarsForQuality(50) == 2);
    CHECK(WifiBarsForQuality(74) == 2);
    CHECK(WifiBarsForQuality(75) == 3);
    CHECK(WifiBarsForQuality(99) == 3);
    CHECK(WifiBarsForQuality(100) == 4);
    // wlanapi cannot report outside 0..100, but a broken reader must degrade, not
    // assert: the clamp keeps the output in range for any input.
    CHECK(WifiBarsForQuality(-1) == 0);
    CHECK(WifiBarsForQuality(101) == 4);
    CHECK(WifiBarsForQuality(1000000) == 4);
}

TEST_CASE("T-306 battery grade: below 20 is the one red band") {
    // The spec pins exactly one alarm colour (HIG systemRed <20%); there is no
    // orange band, so the mapping has exactly two outputs.
    CHECK(BatteryGradeForPercent(0) == UsageGrade::Critical);
    CHECK(BatteryGradeForPercent(19) == UsageGrade::Critical);
    // 20 is the boundary the band closes: "低于 20%" means 20 itself is normal.
    CHECK(BatteryGradeForPercent(20) == UsageGrade::Normal);
    CHECK(BatteryGradeForPercent(21) == UsageGrade::Normal);
    CHECK(BatteryGradeForPercent(87) == UsageGrade::Normal);
    CHECK(BatteryGradeForPercent(100) == UsageGrade::Normal);
    // An impossible reading never paints an alarm (the T-302 degenerate rule).
    CHECK(BatteryGradeForPercent(-5) == UsageGrade::Normal);
}

TEST_CASE("T-306 battery present: BatteryFlag 128 and 255 hide the item") {
    CHECK_FALSE(BatteryPresentForFlag(128));  // BATTERY_FLAG_NO_BATTERY: desktop
    CHECK_FALSE(BatteryPresentForFlag(255));  // BATTERY_FLAG_UNKNOWN: no answer
    // The real flags and their combinations mean a battery exists.
    CHECK(BatteryPresentForFlag(1));   // high
    CHECK(BatteryPresentForFlag(2));   // low
    CHECK(BatteryPresentForFlag(4));   // critical
    CHECK(BatteryPresentForFlag(8));   // charging
    CHECK(BatteryPresentForFlag(9));   // high | charging
    CHECK(BatteryPresentForFlag(11));  // high | low | critical
}

TEST_CASE("T-306 battery charging: the flag bit or AC online") {
    CHECK(BatteryChargingFor(1, 0));       // plugged in (ACLineStatus 1)
    CHECK(BatteryChargingFor(0, 8));       // charging bit without AC read
    CHECK(BatteryChargingFor(1, 8));       // both (the usual charging state)
    CHECK_FALSE(BatteryChargingFor(0, 0));  // on battery, discharging
    CHECK_FALSE(BatteryChargingFor(2, 0));  // ACLineStatus 2 = on backup
}

TEST_CASE("T-306 ime label: the native bit decides between A and 中") {
    CHECK(ImeConversionLabel(0) == L"A");
    // IME_CMODE_NATIVE = 0x1, shared by the Chinese/Japanese/Korean IMEs.
    CHECK(ImeConversionLabel(0x1) == L"\u4E2D");
    // Other bits (FULLSHAPE 0x8, ROMAN 0x10, ...) do not hide the native mode.
    CHECK(ImeConversionLabel(0x1 | 0x8) == L"\u4E2D");
    CHECK(ImeConversionLabel(0x8) == L"A");
}

TEST_CASE("T-306 ime toggle: the native bit flips, every other bit survives") {
    CHECK(ImeToggledConversionMode(0x0) == 0x1);   // A -> 中
    CHECK(ImeToggledConversionMode(0x1) == 0x0);   // 中 -> A
    // FULLSHAPE (0x8) rides along: toggling the script does not reset the width.
    CHECK(ImeToggledConversionMode(0x9) == 0x8);
    CHECK(ImeToggledConversionMode(0x8) == 0x9);
}

TEST_CASE("T-306 connection labels: the popup's kind names") {
    CHECK(std::wstring(ConnectionKindLabel(ConnectionKind::Wifi)) == L"Wi-Fi");
    CHECK(std::wstring(ConnectionKindLabel(ConnectionKind::Ethernet)) == L"\u4EE5\u592A\u7F51");
    CHECK(std::wstring(ConnectionKindLabel(ConnectionKind::None)) == L"\u65E0\u7F51\u7EDC");
}

TEST_CASE("T-306 layout: the battery's 25 DIP icon box and the alert flag") {
    // The battery entry: 12 padding + 25 icon + 4 gap + text, laid from the right
    // edge. The icon rect carries the wider frame; the text rides beside it.
    RightItem battery = RItem(RightType::Battery, 5, L"87%", true, 25.0f);
    battery.alert = true;
    std::vector<RightItem> items{battery};
    const auto boxes = dockforge::finderbar::LayoutRightGroup(items, 1200.0f, 28.0f,
                                                              &FixedWidth, nullptr);
    REQUIRE(boxes.size() == 1);
    // Text "87%" = 3 chars * 13 * 0.5 = 19.5 wide; icon 25 + gap 4 + text 19.5 +
    // padding 12 = 60.5.
    const float text_w = FixedWidth(L"87%", 13.0f);
    const float expected = 12.0f + 25.0f + 4.0f + text_w;
    CHECK(boxes[0].rect.Width() == Catch::Approx(expected).margin(0.01f));
    CHECK(boxes[0].icon.Width() == Catch::Approx(25.0f).margin(0.01f));
    CHECK(boxes[0].icon.Height() == Catch::Approx(16.0f).margin(0.01f));
    CHECK(boxes[0].alert);
    // The icon sits inside the box, before the text (macOS order: glyph, value).
    CHECK(boxes[0].icon.right <= boxes[0].rect.right - 6.0f);
}

TEST_CASE("T-306 layout: icon-only network entry and text-only ime entry") {
    RightItem network = RItem(RightType::Network, 4, L"", true, 16.0f);
    RightItem ime = RItem(RightType::Ime, 6, L"A", false);
    RightItem clock = RItem(RightType::Clock, 0, L"17:27", false);
    std::vector<RightItem> items{network, ime, clock};
    const auto boxes = dockforge::finderbar::LayoutRightGroup(items, 1200.0f, 28.0f,
                                                              &FixedWidth, nullptr);
    // Left-to-right output: ime (order 6) leftmost, then network (4), then the
    // clock (order 0) hard against the right edge - order counts FROM the right
    // edge, so higher order = further left, the macOS reading the ADR freezes.
    REQUIRE(boxes.size() == 3);
    CHECK(boxes[0].type == RightType::Ime);
    CHECK(boxes[1].type == RightType::Network);
    CHECK(boxes[2].type == RightType::Clock);
    CHECK(boxes[1].icon.Width() == Catch::Approx(16.0f).margin(0.01f));
    // Icon-only width: 12 padding + 16 icon (no text, no gap).
    CHECK(boxes[1].rect.Width() == Catch::Approx(28.0f).margin(0.01f));
    // Text-only width: 12 padding + "A" width (no icon, no gap).
    CHECK(boxes[0].rect.Width() ==
          Catch::Approx(12.0f + FixedWidth(L"A", 13.0f)).margin(0.01f));
}

TEST_CASE("T-306 popup height: sections add, an empty card stack is zero") {
    using dockforge::finderbar::StatusPopupHeightFor;
    CHECK(StatusPopupHeightFor(false, false, false) == 0.0f);
    // One section: margins only around the base card.
    CHECK(StatusPopupHeightFor(true, false, false) ==
          Catch::Approx(24.0f + 78.0f).margin(0.01f));
    // All three: + gaps between sections.
    CHECK(StatusPopupHeightFor(true, true, true) ==
          Catch::Approx(24.0f + 78.0f + 78.0f + 62.0f + 20.0f).margin(0.01f));
    // Battery absent (a desktop): the card is shorter, never a hole.
    CHECK(StatusPopupHeightFor(false, true, true) <
          StatusPopupHeightFor(true, true, true));
}

TEST_CASE("T-306 wifi glyph geometry: three arcs, solid bands sized from the slot") {
    using dockforge::finderbar::WifiGlyphGeometry;
    const auto g =
        WifiGlyphGeometry::Compute(dockforge::render::RectF{0.0f, 0.0f, 16.0f, 16.0f});
    CHECK(g.arc_count == 3);
    // ADR-0058: the arcs are SOLID bands - enough overlapping discs that the chain
    // never reads as a dotted line. The band stride is 85% of the disc radius, so
    // the 90-degree arc of the 16 DIP slot lays 11 discs with ~55% overlap.
    CHECK(g.steps_per_arc >= 10);
    // Radii grow and stay inside the slot: the outermost arc's reach along either
    // axis must not exceed the slot's half-extent beyond the apex.
    CHECK(g.radii[0] < g.radii[1]);
    CHECK(g.radii[1] < g.radii[2]);
    CHECK(g.radii[2] <= 16.0f);
    // The apex sits low (the arcs open upward, y-down screen space).
    CHECK(g.apex_y >= 12.0f);
}
