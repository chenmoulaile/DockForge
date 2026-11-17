// DockForge - Finder Bar right-region status sampling (T-306, C-31 ime/battery/network).
//
// The three status indicators the macOS menu bar shows between the tray mirror and
// the clock: the input-method conversion mode ("A" / "中"), the battery (percent +
// charging, hidden on machines without one) and the network (type + signal bars).
//
// DATA CHANNEL (the T-302 precedent, ADR-0054, applies unchanged):
//
//   * Sampling runs on the bar's Tick chain (FinderBar::Tick -> StatusHost::Tick),
//     never inside Paint - the C-23 prohibition targets the render path. Every read
//     here is bounded: GetSystemPowerStatus and GetAdaptersAddresses are
//     microsecond-scale; the one API that can block (SendMessageTimeoutW to another
//     process's default IME window) carries SMTO_ABORTIFHUNG with a 100ms deadline,
//     and WlanQueryInterface is a parameter query, not a scan.
//   * The refresh period is a fixed 1000ms constant (kStatusRefreshMs): the T-306
//     spec pins "刷新 ≤1Hz" as a property of the indicators, not a user knob, so no
//     config key exists and the ceiling cannot be violated by editing the file.
//     This host is independent of the T-302 WidgetHost (whose period is
//     finderbar.widgets_refresh_ms, floor 2000ms).
//
// SOURCES, as the T-306 DoD names them:
//
//   * IME   ImmGetDefaultIMEWnd(foreground) + WM_IME_CONTROL(IMC_GETCONVERSIONMODE);
//     the same window receives WM_IME_CONTROL(IMC_SETCONVERSIONMODE) on a click,
//     which is the DoD's "点击即时切换". The tracked window is whatever the bar is
//     already pinned to (foreground override included) so probes exercise the real
//     path.
//   * Battery GetSystemPowerStatus; BatteryFlag 128 (no battery) / 255 (unknown)
//     means the item is hidden entirely - a desktop must not show a battery it does
//     not have. BatteryLifePercent 255 (unknown) is shown as 0%, the same convention
//     Beautiful's UpdateBattery uses (TopBarWindow.xaml.cs:1119).
//   * Network GetAdaptersAddresses for the connection type (operational adapter with
//     a gateway; IEEE80211 wins over Ethernet) + Network List Manager
//     IsConnectedToInternet for the online flag; Native WiFi
//     WlanQueryInterface(wlan_intf_opcode_current_connection) for the signal.
//     wlanSignalQuality is wlanapi's own 0..100 percentage (not dBm), so the bars
//     mapping divides it in equal quarters - the same pure rule Beautiful's
//     WifiSignal.Bars uses (Beautiful.Core/Network/Wifi.cs:26).
//
// The threshold mapping and the flag decoding are pure functions over numbers, so
// they carry the unit tests (tests/t306_status_tests.inc) the way bar_layout and
// bar_widgets do. The API readers themselves need a live machine and are covered by
// the evidence probes.
#pragma once

#include "core/paths.h"  // core::Utf8ToWide / core::WideToUtf8 (probe JSON + config)
#include "finderbar/bar_widgets.h"  // UsageGrade + the frozen HIG alert colours
#include "platform/win_common.h"
#include "render/backend.h"

#include <string>
#include <vector>

// netlistmgr.h's COM interface, forward-declared at global scope: the member below
// only holds the pointer, the cpp includes the real header. Declared inside the
// dockforge::finderbar namespace it would name a different (nested) type.
struct INetworkListManager;

namespace dockforge::finderbar {

// The fixed sampling period. 1000ms = the spec's 1Hz ceiling exactly; sampling at
// exactly the ceiling means every sample is one second apart, which is what the
// evidence sampling record measures.
constexpr int kStatusRefreshMs = 1000;

// The connection kind the network indicator can show. Declaration order is the
// display priority when several adapters are up: the Wi-Fi glyph is what macOS
// shows when a wireless link is the active route, Ethernet when wired.
enum class ConnectionKind { None = 0, Wifi, Ethernet, };

// One read of all three indicators. A plain value struct: the bar copies it, the
// popup copies it, nothing holds a pointer into it (C-04b).
struct StatusSample {
    // IME. `ime_valid` means the foreground window's default IME window answered a
    // conversion-mode query at least once; false = the item renders nothing (a
    // machine with no IME running shows no input indicator, which is the honest
    // state - macOS also hides its input menu when only one input source exists).
    bool ime_valid = false;
    unsigned ime_conversion_mode = 0;  // raw IMC_GETCONVERSIONMODE answer
    bool ime_native = false;           // mode carries IME_CMODE_NATIVE ("中")

    // Battery. `battery_present` false hides the whole item (BatteryFlag 128/255).
    // percent 0..100; 255 (unknown) is normalized to 0 at sample time.
    bool battery_present = false;
    int battery_percent = 0;
    bool battery_charging = false;
    bool battery_ac_online = false;

    // Network. `network_connected` = an operational adapter with a gateway exists;
    // `network_online` = Network List Manager IsConnectedToInternet. Both false with
    // kind None is the "无网络" state the bar shows dimmed.
    bool network_connected = false;
    bool network_online = false;
    ConnectionKind network_kind = ConnectionKind::None;
    int wifi_quality = 0;              // 0..100 from wlanapi (100 = best)
    int wifi_bars = 0;                 // 0..4, the pure mapping below
    std::wstring wifi_ssid;            // popup detail; empty when not on Wi-Fi
};

// ---- pure mappings (unit tested) ---------------------------------------------

// Signal bars 0..4 for wlanapi's 0..100 signal quality. wlanSignalQuality is a
// linear percentage, so four equal bands are the honest scale and the band edges
// land on tens so the mapping can be checked by mental arithmetic. Thresholds are
// the ones Beautiful ships (Wifi.cs:26), read as measurements of the same API, not
// copied pixels. Values outside 0..100 clamp first.
int WifiBarsForQuality(int quality);

// The battery colour tier: macOS HIG systemRed (#FF453A, the frozen
// kWidgetCriticalColor) when the charge is below 20%, the theme foreground
// otherwise. The spec pins exactly one alarm band, so the mapping has exactly two
// outputs; a made-up orange band would imply a threshold nobody specified.
UsageGrade BatteryGradeForPercent(int percent);

// BatteryFlag decoding. 128 (BATTERY_FLAG_NO_BATTERY) and 255 (BATTERY_FLAG_UNKNOWN)
// mean "no battery to show"; every other flag (1 high / 2 low / 4 critical /
// 8 charging, and combinations) means one is present.
bool BatteryPresentForFlag(unsigned battery_flag);

// Charging = the flag's 8 bit, or the machine being plugged in (ACLineStatus 1 -
// a charging battery reports both, but a dead battery on AC still shows the bolt).
bool BatteryChargingFor(unsigned ac_line_status, unsigned battery_flag);

// The conversion-mode label: IME_CMODE_NATIVE (0x1, the same bit the IMEs of every
// CJK language share) shows "中", anything else - including the alphanumeric mode
// with full-shape or soft-keyboard bits set - shows "A".
std::wstring ImeConversionLabel(unsigned conversion_mode);

// The mode a click switches TO: the native bit flipped, every other bit kept.
// IMC_SETCONVERSIONMODE with this value is the whole click action (T-306 DoD).
unsigned ImeToggledConversionMode(unsigned conversion_mode);

// The human label for a connection kind (popup detail + probe JSON).
const wchar_t* ConnectionKindLabel(ConnectionKind kind);

// ---- StatusHost --------------------------------------------------------------

// One StatusHost per bar; the bar owns it. Like WidgetHost but without worker
// threads: every sample is bounded (see the header comment), so the whole thing
// lives on the Tick chain.
class StatusHost {
  public:
    StatusHost() = default;
    ~StatusHost() = default;
    StatusHost(const StatusHost&) = delete;
    StatusHost& operator=(const StatusHost&) = delete;

    // Arms the host when the configured right table actually renders one of the
    // three status types; otherwise stops everything. Idempotent - ReloadConfig
    // calls it on every reload, and a no-op must cost nothing.
    void Start(const core::FinderBarConfig& config);
    void Stop();
    bool Running() const { return running_; }

    // Advances the sampler. Called from FinderBar::Tick on the main thread with the
    // window the bar is currently tracking (the foreground, or the probe pin).
    // Returns true when anything the right region renders changed - the caller
    // re-lays the bar and repaints.
    bool Tick(double delta_ms, HWND tracked_window);

    // The click action on the ime item: sends WM_IME_CONTROL(IMC_SETCONVERSIONMODE)
    // with ImeToggledConversionMode(current) to the tracked window's default IME
    // window and re-samples immediately, so the label flips in the same frame rather
    // than at the next 1s tick. Returns false when no IME window answered.
    bool ToggleImeConversionMode(HWND tracked_window);

    // Forces an immediate re-sample (probe path; also what the toggle uses).
    void SampleNow(HWND tracked_window);

    const StatusSample& Sample() const { return sample_; }

    // One-line JSON for the bar's probe state file: every field the right region
    // renders plus the sampling bookkeeping the 1Hz evidence record asserts on.
    std::string SnapshotJson() const;

    // Sampling counters. `samples` grows once per period while the bar is visible;
    // the evidence record diffs two state reads against wall clock to prove <=1Hz.
    // The environment hook (below) intentionally also counts as samples.
    std::uint64_t samples() const { return samples_; }
    std::uint64_t last_sample_tick_ms() const { return last_sample_tick_ms_; }

  private:
    void Sample(HWND tracked_window);

    StatusSample sample_{};         // the live state (Tick chain only)
    StatusSample published_{};      // what the last "changed" answer was computed from
    bool has_published_ = false;
    double accumulator_ms_ = 0.0;
    bool running_ = false;

    // Network List Manager object created lazily on the Tick thread (the main
    // thread, where OleInitialize already ran - main.cpp:387). Null = the query is
    // unavailable and `network_online` follows the adapter gateway answer instead.
    INetworkListManager* net_list_manager_ = nullptr;
    bool net_list_manager_tried_ = false;

    // Native WiFi handle. Opened once; a machine without the WLAN service stays
    // null and the Wi-Fi strength simply reads 0 (an Ethernet-only desktop has no
    // wireless strength to show - that is not an error to report).
    void* wlan_handle_ = nullptr;
    bool wlan_open_tried_ = false;

    std::uint64_t samples_ = 0;
    std::uint64_t last_sample_tick_ms_ = 0;
};

// Evidence hook, the DOCKFORGE_WIDGETS_DEMO precedent: replaces sampled fields with
// fixed ones so the two battery states (a desktop has no battery to charge or drain)
// and specific Wi-Fi strengths are photographable deterministically. Accepted
// values:
//
//   battery   -> battery present, 87%, charging, AC online
//   low       -> battery present, 15%, discharging (the red state)
//   nobattery -> battery absent (the hidden state, deterministic on any machine)
//   wifi4     -> Wi-Fi connected, quality 100 (4 lit arcs)
//   wifi2     -> Wi-Fi connected, quality 60 (2 lit arcs)
//   offline   -> not connected, no internet
//
// One value per run (they are mutually exclusive states, not layers). Active mode is
// logged as a WARN by the host; default runs never set it.
bool ApplyStatusDemoIfAny(StatusSample* sample);

}  // namespace dockforge::finderbar
