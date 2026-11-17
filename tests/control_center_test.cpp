// DockForge - T-304 control centre unit tests (Catch2).
//
// What is testable without a window (the HWND-bound surface is covered by the
// evidence probe):
//   - the Sonoma height arithmetic (Beautiful ControlCenterPanel.xaml metrics)
//   - the capsule slider level math (vertical fills bottom-up, horizontal left-to-right)
//   - the 勿扰 toast-value semantics
//   - the 夜览 CloudStore blob byte contract (validation / state / flip)
//   - the right-click minimal menu table (ms-settings URIs, ADR-0039)
//   - the right-region registration + default table insertion (ADR-0057)
#include <catch_amalgamated.hpp>

#include "core/config.h"
#include "finderbar/bar_layout.h"
#include "finderbar/hardware_panel.h"

TEST_CASE("T-304 panel height arithmetic pins the Sonoma stack", "[finderbar]") {
    using namespace dockforge::finderbar;
    const float h = HardwarePanelHeightFor(true);
    // Top pad 10 + top area 168 + gap 8 + brightness row 46 + gap 8 + error 22 +
    // bottom pad 12 = 274.
    CHECK(h == 274.0f);
    // The brightness row exists in both states (the hint replaces the percent
    // inside the row), so an unsupported machine keeps the same card.
    CHECK(HardwarePanelHeightFor(false) == h);
    // The width is the Beautiful Card Width=348 metric.
    CHECK(kCcPanelWidthDip == 348.0f);
    // Panel radius = MacOS.xaml ThemePanelCornerRadius; card radii from
    // ControlCenterPanel.xaml (CcConnCard 16 / CcGroupStyle 12 / CcSliderTile 14).
    CHECK(kCcPanelRadiusDip == 18.0f);
    CHECK(kCcCardRadiusDip == 16.0f);
    CHECK(kCcGroupRadiusDip == 12.0f);
    CHECK(kCcTileRadiusDip == 14.0f);
    // Connection button 28x28; slider 26x118 with the 20-wide visible capsule and
    // the 4 DIP fill-edge handle - the ask's metric record.
    CHECK(kCcConnButtonDip == 28.0f);
    CHECK(kCcSliderWidthDip == 26.0f);
    CHECK(kCcSliderTrackWidthDip == 20.0f);
    CHECK(kCcSliderLengthDip == 118.0f);
    CHECK(kCcThumbDip == 4.0f);
}

TEST_CASE("T-304 vertical slider level fills from the bottom and clamps", "[finderbar]") {
    using namespace dockforge::finderbar;
    CHECK(ControlCenterSliderLevelFromY(0.0f, 0.0f, 100.0f) == 1.0f);    // top = full
    CHECK(ControlCenterSliderLevelFromY(100.0f, 0.0f, 100.0f) == 0.0f);  // bottom = empty
    CHECK(ControlCenterSliderLevelFromY(50.0f, 0.0f, 100.0f) == 0.5f);
    // Outside the track clamps instead of extrapolating.
    CHECK(ControlCenterSliderLevelFromY(-10.0f, 0.0f, 100.0f) == 1.0f);
    CHECK(ControlCenterSliderLevelFromY(110.0f, 0.0f, 100.0f) == 0.0f);
    // Degenerate track reads 0.
    CHECK(ControlCenterSliderLevelFromY(5.0f, 0.0f, 0.0f) == 0.0f);
}

TEST_CASE("T-304 horizontal slider level fills from the left and clamps", "[finderbar]") {
    using namespace dockforge::finderbar;
    CHECK(ControlCenterSliderLevelFromX(0.0f, 0.0f, 100.0f) == 0.0f);
    CHECK(ControlCenterSliderLevelFromX(100.0f, 0.0f, 100.0f) == 1.0f);
    CHECK(ControlCenterSliderLevelFromX(25.0f, 0.0f, 100.0f) == 0.25f);
    CHECK(ControlCenterSliderLevelFromX(-5.0f, 0.0f, 100.0f) == 0.0f);
    CHECK(ControlCenterSliderLevelFromX(105.0f, 0.0f, 100.0f) == 1.0f);
}

TEST_CASE("T-304 勿扰 semantics: toast master switch maps to the toggle", "[finderbar]") {
    using namespace dockforge::finderbar;
    // 0 = toasts off = DND on; missing/nonzero = toasts on = DND off.
    CHECK(CcDndOnFromToastValue(true, 0) == true);
    CHECK(CcDndOnFromToastValue(true, 1) == false);
    CHECK(CcDndOnFromToastValue(false, 0) == false);  // absent value = default off
}

TEST_CASE("T-304 夜览 blob contract: validate, read state, flip", "[finderbar]") {
    using namespace dockforge::finderbar;
    // The documented CloudStore shape: "CB" magic + the enable byte at 0x12.
    std::uint8_t blob[0x20] = {'C', 'B'};
    blob[0x12] = kCcNightByteOff;
    CHECK(CcNightBlobLooksValid(blob, sizeof(blob)));
    CHECK(CcNightStateFromBlob(blob, sizeof(blob)) == -1);

    blob[0x12] = kCcNightByteOn;
    CHECK(CcNightStateFromBlob(blob, sizeof(blob)) == 1);

    // Flip toggles between the two documented bytes and back.
    CHECK(CcNightFlippedEnableByte(kCcNightByteOn) == kCcNightByteOff);
    CHECK(CcNightFlippedEnableByte(kCcNightByteOff) == kCcNightByteOn);

    // Rejects: too short, wrong magic, unrecognized enable byte.
    CHECK_FALSE(CcNightBlobLooksValid(blob, 0x10));
    std::uint8_t bad_magic[0x20] = {'X', 'Y'};
    bad_magic[0x12] = kCcNightByteOn;
    CHECK_FALSE(CcNightBlobLooksValid(bad_magic, sizeof(bad_magic)));
    std::uint8_t bad_byte[0x20] = {'C', 'B'};
    bad_byte[0x12] = 0x99;
    CHECK_FALSE(CcNightBlobLooksValid(bad_byte, sizeof(bad_byte)));
    CHECK(CcNightStateFromBlob(bad_byte, sizeof(bad_byte)) == 0);
    CHECK(CcNightStateFromBlob(nullptr, 0) == 0);
}

TEST_CASE("T-304 minimal menu table: ADR-0039 aggregation pages", "[finderbar]") {
    using namespace dockforge::finderbar;
    CHECK(std::wstring(CcSettingsPageUri(CcSettingsPage::Sound)) == L"ms-settings:sound");
    CHECK(std::wstring(CcSettingsPageUri(CcSettingsPage::Display)) == L"ms-settings:display");
    CHECK(std::wstring(CcSettingsPageUri(CcSettingsPage::Bluetooth)) ==
          L"ms-settings:bluetooth");
    CHECK(std::wstring(CcSettingsPageUri(CcSettingsPage::Network)) ==
          L"ms-settings:network-wifi");
    CHECK(std::wstring(CcSettingsPageUri(CcSettingsPage::Notifications)) ==
          L"ms-settings:notifications");
    CHECK(std::wstring(CcSettingsPageUri(CcSettingsPage::All)) == L"ms-settings:");
    // Every page has a label; an invalid page has neither.
    for (int i = 1; i <= 6; ++i) {
        const auto page = static_cast<CcSettingsPage>(i);
        CHECK(CcSettingsPageUri(page) != nullptr);
        CHECK(CcSettingsPageLabel(page) != nullptr);
        CHECK(std::wstring(CcSettingsPageLabel(page)).size() > 0);
    }
    CHECK(CcSettingsPageUri(static_cast<CcSettingsPage>(0)) == nullptr);
    CHECK(CcSettingsPageLabel(static_cast<CcSettingsPage>(99)) == nullptr);
}

TEST_CASE("T-304 control centre registers in the right region and the default table",
          "[finderbar]") {
    using namespace dockforge::finderbar;
    // bar_layout: the type flips registered (it renders as the dual-toggle glyph).
    CHECK(RightTypeRegistered(RightType::ControlCenter));
    // C-31 spelling round-trips through the name table.
    CHECK(RightTypeFromName(L"control_center").has_value());
    CHECK(*RightTypeFromName(L"control_center") == RightType::ControlCenter);
    // ADR-0057: the frozen default table inserts control_center at order=1
    // (clock=0 stays rightmost; macOS reading order clock <- control centre).
    const auto defaults = dockforge::core::DefaultFinderBarRightItems();
    bool found = false;
    for (const auto& item : defaults) {
        if (item.type == "control_center") {
            found = true;
            CHECK(item.order == 1);
        }
    }
    CHECK(found);
    // ADR-0058 completed the pre-agreed table with notification_center=2 and
    // tray_mirror=3, then extended the C-31 enum additively with volume=7 and
    // spotlight=8: clock, control_center, notification_center, tray_mirror,
    // network, battery, ime, volume, spotlight.
    CHECK(defaults.size() == 9);
}
