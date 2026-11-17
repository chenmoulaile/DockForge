// ---------------------------------------------------------------------------
// T-302: the centre widget zone (finderbar.widgets) - grade mapping, the frozen
// alert colours, the display text, the pure sampling helpers and the centre-row
// layout. The sampling APIs themselves (GetIfTable2 / GetDiskFreeSpaceExW / WMI)
// need a live machine and are covered by the evidence probes, not here.
// ---------------------------------------------------------------------------

#include "finderbar/bar_widgets.h"

#include <limits>

namespace {

using dockforge::finderbar::UsageGrade;
using dockforge::finderbar::WidgetItem;
using dockforge::finderbar::WidgetType;

// Builds one centre entry with a text of a known fake width (FixedWidth above).
WidgetItem WItem(WidgetType type, int order, const std::wstring& text) {
    WidgetItem item;
    item.type = type;
    item.order = order;
    item.text = text;
    return item;
}

dockforge::core::FinderBarWidgetItem ConfigWidget(const char* type, int order) {
    dockforge::core::FinderBarWidgetItem entry;
    entry.type = type;
    entry.order = order;
    return entry;
}

}  // namespace

TEST_CASE("T-302 grade: the three tiers split at 80 and 90, boundary values included") {
    using dockforge::finderbar::GradeForPercent;
    CHECK(GradeForPercent(0.0f) == UsageGrade::Normal);
    CHECK(GradeForPercent(42.0f) == UsageGrade::Normal);
    CHECK(GradeForPercent(79.999f) == UsageGrade::Normal);
    // 80 is warn's lower edge (the spec grades "80-90" orange, "<80" default).
    CHECK(GradeForPercent(80.0f) == UsageGrade::Warn);
    CHECK(GradeForPercent(85.0f) == UsageGrade::Warn);
    // 90 is warn's upper edge; only >90 is red.
    CHECK(GradeForPercent(90.0f) == UsageGrade::Warn);
    CHECK(GradeForPercent(90.001f) == UsageGrade::Critical);
    CHECK(GradeForPercent(100.0f) == UsageGrade::Critical);
    // Degenerate inputs grade Normal: an impossible reading never paints an alarm.
    CHECK(GradeForPercent(-5.0f) == UsageGrade::Normal);
    CHECK(GradeForPercent(std::numeric_limits<float>::quiet_NaN()) == UsageGrade::Normal);
}

TEST_CASE("T-302 grade: the alert colours are the frozen HIG hex values") {
    // #FF9F0A = (255,159,10); #FF453A = (255,69,58). Taken from macOS HIG
    // systemOrange/systemRed - Beautiful's MacOS.xaml has no orange/red keys
    // (grep 9F0A / 453A / Orange / Red = 0 hits, verified for this task), so
    // there is nothing to copy from there.
    const dockforge::render::ColorF warn =
        dockforge::finderbar::WidgetGradeColor(UsageGrade::Warn);
    CHECK(warn.r == 1.0f);
    CHECK(warn.g == 159.0f / 255.0f);
    CHECK(warn.b == 10.0f / 255.0f);
    const dockforge::render::ColorF crit =
        dockforge::finderbar::WidgetGradeColor(UsageGrade::Critical);
    CHECK(crit.r == 1.0f);
    CHECK(crit.g == 69.0f / 255.0f);
    CHECK(crit.b == 58.0f / 255.0f);
    // Normal returns the sentinel the painter replaces with the theme colour.
    const dockforge::render::ColorF normal =
        dockforge::finderbar::WidgetGradeColor(UsageGrade::Normal);
    CHECK(normal.a == 0.0f);
}

TEST_CASE("T-302 registry: the eight C-31 names round-trip; five render today") {
    using dockforge::finderbar::WidgetTypeFromName;
    using dockforge::finderbar::WidgetTypeName;
    for (int i = 0; i < dockforge::finderbar::kWidgetTypeCount; ++i) {
        const auto parsed = WidgetTypeFromName(WidgetTypeName(i));
        REQUIRE(parsed.has_value());
        CHECK(static_cast<int>(*parsed) == i);
    }
    CHECK_FALSE(WidgetTypeFromName(L"widget").has_value());
    CHECK_FALSE(WidgetTypeFromName(L"").has_value());
    CHECK_FALSE(WidgetTypeFromName(nullptr).has_value());

    // This package registers exactly the five monitoring widgets; weather /
    // media / calendar_brief wait for T-302d and cost no box until then.
    CHECK(dockforge::finderbar::WidgetTypeRegistered(WidgetType::Cpu));
    CHECK(dockforge::finderbar::WidgetTypeRegistered(WidgetType::Memory));
    CHECK(dockforge::finderbar::WidgetTypeRegistered(WidgetType::Disk));
    CHECK(dockforge::finderbar::WidgetTypeRegistered(WidgetType::Network));
    CHECK(dockforge::finderbar::WidgetTypeRegistered(WidgetType::Temperature));
    CHECK_FALSE(dockforge::finderbar::WidgetTypeRegistered(WidgetType::Weather));
    CHECK_FALSE(dockforge::finderbar::WidgetTypeRegistered(WidgetType::Media));
    CHECK_FALSE(dockforge::finderbar::WidgetTypeRegistered(WidgetType::CalendarBrief));
}

TEST_CASE("T-302 text: the labels are compact single lines in the bar's font size") {
    using dockforge::finderbar::WidgetDisplayText;
    dockforge::finderbar::WidgetSnapshot s;
    s.cpu_percent = 42.4f;
    s.memory_percent = 63.5f;
    s.disk_percent = 81.2f;
    s.network_bytes_per_sec = 375000.0;
    s.temperature_valid = true;
    s.temperature_c = 45.6f;
    CHECK(WidgetDisplayText(WidgetType::Cpu, s) == L"CPU 42%");
    CHECK(WidgetDisplayText(WidgetType::Memory, s) == L"MEM 64%");
    CHECK(WidgetDisplayText(WidgetType::Disk, s) == L"DISK 81%");
    // Rates: whole KB/s below a megabyte, one decimal in MB/s above it.
    CHECK(WidgetDisplayText(WidgetType::Network, s) == L"NET 375KB/s");
    s.network_bytes_per_sec = 2400000.0;
    CHECK(WidgetDisplayText(WidgetType::Network, s) == L"NET 2.4MB/s");
    s.network_bytes_per_sec = 0.0;
    // Zero traffic still prints (blank would be indistinguishable from broken).
    CHECK(WidgetDisplayText(WidgetType::Network, s) == L"NET 0KB/s");
    CHECK(WidgetDisplayText(WidgetType::Temperature, s) == L"TEMP 46\xB0" L"C");
    // An unusable sensor renders NOTHING - never a fabricated number.
    s.temperature_valid = false;
    CHECK(WidgetDisplayText(WidgetType::Temperature, s).empty());
    // T-302d types render nothing this package.
    CHECK(WidgetDisplayText(WidgetType::Weather, s).empty());
}

TEST_CASE("T-302 sampling: disk percent and network rate are pure and edge-safe") {
    using dockforge::finderbar::ComputeDiskPercent;
    using dockforge::finderbar::ComputeNetworkRate;
    CHECK(ComputeDiskPercent(0, 100) == 0.0f);
    CHECK(ComputeDiskPercent(50, 100) == 50.0f);
    CHECK(ComputeDiskPercent(100, 100) == 100.0f);
    CHECK(ComputeDiskPercent(150, 100) == 100.0f);  // clamp
    CHECK(ComputeDiskPercent(10, 0) == 0.0f);       // degenerate total

    CHECK(ComputeNetworkRate(100, 200, 2.0) == 50.0);   // bytes per second
    CHECK(ComputeNetworkRate(100, 200, 0.0) == 0.0);    // no elapsed time
    CHECK(ComputeNetworkRate(100, 200, -1.0) == 0.0);   // clock skew
    CHECK(ComputeNetworkRate(200, 100, 2.0) == 0.0);    // adapter reset -> 0, not negative
    CHECK(ComputeNetworkRate(100, 100, 2.0) == 0.0);    // idle link
}

TEST_CASE("T-302 build: unregistered types and empty labels produce no item") {
    dockforge::core::FinderBarConfig config;
    config.widgets = {ConfigWidget("cpu", 0), ConfigWidget("weather", 1),
                      ConfigWidget("temperature", 2), ConfigWidget("garbage", 3)};
    dockforge::finderbar::WidgetSnapshot s;
    s.cpu_percent = 10.0f;
    // Temperature invalid -> its label is empty -> the item is dropped, so an
    // unavailable sensor renders as nothing at all (DoD).
    const auto items = dockforge::finderbar::BuildWidgetItems(config, s);
    REQUIRE(items.size() == 1);
    CHECK(items[0].type == WidgetType::Cpu);

    // A valid sensor turns the temperature item back on, with the grade the
    // snapshot implies (85C is the Warn tier; the same thresholds as percent,
    // read as degrees Celsius).
    s.temperature_valid = true;
    s.temperature_c = 85.0f;
    const auto with_temp = dockforge::finderbar::BuildWidgetItems(config, s);
    REQUIRE(with_temp.size() == 2);
    CHECK(with_temp[1].type == WidgetType::Temperature);
    CHECK(with_temp[1].grade == UsageGrade::Warn);
    CHECK(dockforge::finderbar::WidgetTypeName(static_cast<int>(with_temp[1].type)) ==
          std::wstring(L"temperature"));
}

TEST_CASE("T-302 build: percent widgets carry the grade of their snapshot") {
    dockforge::core::FinderBarConfig config;
    config.widgets = {ConfigWidget("cpu", 0), ConfigWidget("memory", 1),
                      ConfigWidget("disk", 2), ConfigWidget("network", 3)};
    dockforge::finderbar::WidgetSnapshot s;
    s.cpu_percent = 95.0f;      // critical
    s.memory_percent = 85.0f;   // warn
    s.disk_percent = 42.0f;     // normal
    // Network is a rate, not a fraction - it always renders in the theme colour.
    s.network_bytes_per_sec = 9000000.0;
    const auto items = dockforge::finderbar::BuildWidgetItems(config, s);
    REQUIRE(items.size() == 4);
    CHECK(items[0].grade == UsageGrade::Critical);
    CHECK(items[1].grade == UsageGrade::Warn);
    CHECK(items[2].grade == UsageGrade::Normal);
    CHECK(items[3].grade == UsageGrade::Normal);
}

TEST_CASE("T-302 layout: the row centres in the free span between the two groups") {
    // With the fake measurer (0.5 em per char at 13 DIP = 6.5/char) every label is
    // arithmetic a reader can redo: text + 2 * kWidgetContentPaddingDip.
    const float padding = dockforge::finderbar::kWidgetContentPaddingDip;
    const float gap = dockforge::finderbar::kWidgetGapDip;
    std::vector<WidgetItem> items = {WItem(WidgetType::Cpu, 0, L"CPU 42%"),       // 45.5 + 12
                                     WItem(WidgetType::Memory, 1, L"MEM 63%"),    // 45.5 + 12
                                     WItem(WidgetType::Disk, 2, L"DISK 55%")};    // 52.0 + 12
    dockforge::finderbar::WidgetGroupSummary summary;
    const auto boxes = dockforge::finderbar::LayoutWidgetGroup(items, 28.0f, 100.0f, 1100.0f,
                                                               &FixedWidth, &summary);
    REQUIRE(boxes.size() == 3);

    const float w_cpu = 2 * padding + 7 * 6.5f;   // 57.5
    const float w_mem = w_cpu;                    // "MEM 63%" is also 7 chars
    const float w_disk = 2 * padding + 8 * 6.5f;  // 64.0
    const float total = w_cpu + gap + w_mem + gap + w_disk;

    // Free span [100, 1100]: centre 600, anchor 600 - total/2.
    const float span_centre = (100.0f + 1100.0f) * 0.5f;
    const float anchor = span_centre - total * 0.5f;
    CHECK(summary.start_dip == anchor);
    CHECK(boxes[0].rect.left == anchor);
    CHECK(boxes[0].rect.right == anchor + w_cpu);
    CHECK(boxes[1].rect.left == anchor + w_cpu + gap);
    CHECK(boxes[2].rect.left == anchor + w_cpu + gap + w_mem + gap);
    CHECK(summary.total_width_dip == total);
    CHECK(summary.placed == 3);
    CHECK(summary.dropped == 0);
    // Full bar height, like every other box in this bar.
    for (const auto& box : boxes) {
        CHECK(box.rect.top == 0.0f);
        CHECK(box.rect.bottom == 28.0f);
        CHECK(box.rect.right <= 1100.0f);
        CHECK(box.rect.left >= 100.0f);
    }
}

TEST_CASE("T-302 layout: order counts from the left, ties follow the C-31 declaration") {
    std::vector<WidgetItem> items = {WItem(WidgetType::Temperature, 2, L"TEMP 45\xB0" L"C"),
                                     WItem(WidgetType::Cpu, 0, L"CPU 42%"),
                                     WItem(WidgetType::Disk, 2, L"DISK 55%"),
                                     WItem(WidgetType::Memory, 1, L"MEM 63%")};
    dockforge::finderbar::WidgetGroupSummary summary;
    const auto boxes = dockforge::finderbar::LayoutWidgetGroup(items, 28.0f, 0.0f, 2000.0f,
                                                               &FixedWidth, &summary);
    REQUIRE(boxes.size() == 4);
    // order 0 first, then 1, then the two order-2 entries in declaration order
    // (disk, enum 2, before temperature, enum 4).
    CHECK(boxes[0].type == WidgetType::Cpu);
    CHECK(boxes[1].type == WidgetType::Memory);
    CHECK(boxes[2].type == WidgetType::Disk);
    CHECK(boxes[3].type == WidgetType::Temperature);
}

TEST_CASE("T-302 layout: an entry crossing the right limit is dropped, not squeezed") {
    std::vector<WidgetItem> items = {WItem(WidgetType::Cpu, 0, L"CPU 42%"),
                                     WItem(WidgetType::Memory, 1, L"MEM 63%"),
                                     WItem(WidgetType::Disk, 2, L"DISK 55%")};
    dockforge::finderbar::WidgetGroupSummary summary;
    // 2 * 57.5 + 1 * 12 = 127 fits into 150; the third box (another 12 + 64)
    // crosses the limit and is dropped.
    const auto boxes = dockforge::finderbar::LayoutWidgetGroup(items, 28.0f, 0.0f, 150.0f,
                                                               &FixedWidth, &summary);
    CHECK(boxes.size() == 2);
    CHECK(summary.placed == 2);
    CHECK(summary.dropped == 1);
    for (const auto& box : boxes) CHECK(box.rect.right <= 150.0f);
    // The reported width shrinks to what was placed, not what was measured.
    CHECK(summary.total_width_dip == boxes.back().rect.right - boxes.front().rect.left);
}

TEST_CASE("T-302 layout: a row wider than the whole span clamps to the left limit") {
    std::vector<WidgetItem> items;
    std::wstring wide(20, L'W');  // 20 * 6.5 + 12 = 142 per box
    for (int i = 0; i < 5; ++i) items.push_back(WItem(WidgetType::Cpu, i, wide));
    dockforge::finderbar::WidgetGroupSummary summary;
    const auto boxes = dockforge::finderbar::LayoutWidgetGroup(items, 28.0f, 100.0f, 300.0f,
                                                               &FixedWidth, &summary);
    // 5 * 142 + 4 * 12 = 758 DIP against a 200 DIP span: the anchor collapses to
    // the left limit and entries drop as they cross the right edge.
    CHECK(summary.start_dip == 100.0f);
    CHECK(summary.dropped > 0);
    for (const auto& box : boxes) CHECK(box.rect.right <= 300.0f);
}

TEST_CASE("T-302 layout: a null measurer falls back to the approximation") {
    std::vector<WidgetItem> items = {WItem(WidgetType::Cpu, 0, L"CPU 42%")};
    dockforge::finderbar::WidgetGroupSummary summary;
    const auto boxes = dockforge::finderbar::LayoutWidgetGroup(items, 28.0f, 0.0f, 800.0f,
                                                               nullptr, &summary);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].rect.Width() > 0.0f);
}

TEST_CASE("T-302 config: the default table is the five monitoring widgets") {
    dockforge::core::Config config;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig("{}", config, &error));
    CHECK(error.empty());
    REQUIRE(config.finderbar.widgets.size() == 5);
    CHECK(config.finderbar.widgets[0].type == "cpu");
    CHECK(config.finderbar.widgets[0].order == 0);
    CHECK(config.finderbar.widgets[1].type == "memory");
    CHECK(config.finderbar.widgets[2].type == "disk");
    CHECK(config.finderbar.widgets[3].type == "network");
    CHECK(config.finderbar.widgets[4].type == "temperature");
    CHECK(config.finderbar.widgets[4].order == 4);
    CHECK(config.finderbar.widgets_refresh_ms == 2000);
    CHECK(dockforge::core::SameWidgetItems(config.finderbar.widgets,
                                           dockforge::core::DefaultFinderBarWidgets()));
}

TEST_CASE("T-302 config: finderbar.widgets and widgets_refresh_ms round-trip") {
    // An unknown-but-nonempty type string is user data and survives untouched
    // (C-01), exactly like the right table's rule; only a TYPELESS entry is
    // dropped, because nothing could ever render it.
    const std::string json =
        "{\"finderbar\":{\"widgets\":[{\"type\":\"cpu\",\"order\":0},"
        "{\"type\":\"media\",\"order\":1},{\"type\":\"garbage\",\"order\":2},"
        "{\"type\":\"temperature\",\"order\":3}],\"widgets_refresh_ms\":5000}}";
    dockforge::core::Config config;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig(json, config, &error));
    REQUIRE(config.finderbar.widgets.size() == 4);
    CHECK(config.finderbar.widgets[0].type == "cpu");
    CHECK(config.finderbar.widgets[1].type == "media");
    CHECK(config.finderbar.widgets[2].type == "garbage");
    CHECK(config.finderbar.widgets[3].type == "temperature");
    CHECK(config.finderbar.widgets[3].order == 3);
    CHECK(config.finderbar.widgets_refresh_ms == 5000);

    dockforge::core::Config round_trip;
    REQUIRE(dockforge::core::ParseConfig(dockforge::core::SerializeConfig(config), round_trip,
                                         &error));
    CHECK(dockforge::core::SameWidgetItems(round_trip.finderbar.widgets, config.finderbar.widgets));
    CHECK(round_trip.finderbar.widgets_refresh_ms == 5000);
}

TEST_CASE("T-302 config: an explicit empty widgets table stays empty") {
    // The one deliberate difference from the right table: the centre zone is
    // optional chrome, so "widgets": [] means "no widgets" and must survive the
    // round-trip instead of being refilled with defaults.
    dockforge::core::Config config;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig("{\"finderbar\":{\"widgets\":[]}}", config, &error));
    CHECK(config.finderbar.widgets.empty());
    dockforge::core::Config round_trip;
    REQUIRE(dockforge::core::ParseConfig(dockforge::core::SerializeConfig(config), round_trip,
                                         &error));
    CHECK(round_trip.finderbar.widgets.empty());
}

TEST_CASE("T-302 config: the refresh period clamps to C-23's 0.5Hz ceiling") {
    dockforge::core::Config fast;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig("{\"finderbar\":{\"widgets_refresh_ms\":500}}", fast,
                                         &error));
    CHECK(fast.finderbar.widgets_refresh_ms == 2000);
    dockforge::core::Config slow;
    REQUIRE(dockforge::core::ParseConfig("{\"finderbar\":{\"widgets_refresh_ms\":120000}}", slow,
                                         &error));
    CHECK(slow.finderbar.widgets_refresh_ms == 60000);
}

TEST_CASE("T-302 config: default widgets write no finderbar key at all") {
    // The C-01 quiet-file rule, now covering the two new keys.
    dockforge::core::Config config;
    std::string error;
    REQUIRE(dockforge::core::ParseConfig("{}", config, &error));
    const std::string serialized = dockforge::core::SerializeConfig(config);
    CHECK(serialized.find("\"finderbar\"") == std::string::npos);
    // A refresh-period change brings exactly that key back (and no widgets key -
    // the table is still the default).
    config.finderbar.widgets_refresh_ms = 4000;
    const std::string tweaked = dockforge::core::SerializeConfig(config);
    CHECK(tweaked.find("\"widgets_refresh_ms\"") != std::string::npos);
    CHECK(tweaked.find("\"widgets\"") == std::string::npos);
}
