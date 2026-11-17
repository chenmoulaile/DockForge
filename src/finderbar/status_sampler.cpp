// DockForge - Finder Bar right-region status sampling implementation (T-306).
//
// The pure mappings live at the top (unit tested); the API readers are the thin,
// bounded wrappers the header comment describes. Everything here runs on the bar's
// Tick chain (main thread), never inside Paint.
#include "finderbar/status_sampler.h"

#include "core/log.h"

// MinGW include-order shim for the MIB2 APIs - identical to bar_widgets.cpp's,
// which documents why the four headers below must precede iphlpapi.h (iphlpapi.h
// pulls netioapi.h with its guard already set, and netioapi.h's own MIB2 block
// then goes undeclared unless winsock2/ws2ipdef/nldef/ifdef supplied what it
// needs first). WIN32_LEAN_AND_MEAN means windows.h brought no winsock either.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <nldef.h>
#include <ifdef.h>
#include <iphlpapi.h>

#include <imm.h>
#include <netlistmgr.h>
#include <wlanapi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

namespace dockforge::finderbar {
namespace {

constexpr char kModule[] = "finderbar";

// The conversion-mode sub-commands of WM_IME_CONTROL. MinGW's public imm.h starts
// its IMC_* list at IMC_GETCANDIDATEPOS=0x0007; the conversion-mode commands are
// the head of the same list, pinned by the Windows SDK immdev.h (SETCONVERSIONMODE
// 0x0002, SETSENTENCEMODE 0x0004, SETOPENSTATUS 0x0006 - the "for NI_CONTEXTUPDATED"
// block) and the GET/SET alternation (GETSENTENCEMODE 0x0003, GETOPENSTATUS 0x0005).
// Spikes/T-306_status/ime_probe.cpp verified empirically on this machine that
// 0x0001 reads the conversion mode (0x0 in alphanumeric mode), 0x0002 flips it to
// 0x1 and the read-back matches, and 0x0003 answers a different quantity (0x8) -
// so the mapping is not guesswork.
constexpr WPARAM kImcGetConversionMode = 0x0001;
constexpr WPARAM kImcSetConversionMode = 0x0002;

// The one bit every CJK IME uses for "typing the national script" (IME_CMODE_NATIVE
// = IME_CMODE_CHINESE = IME_CMODE_JAPANESE = IME_CMODE_HANGEUL in imm.h).
constexpr unsigned kImeCmodeNative = 0x1u;

// BatteryFlag / ACLineStatus bits (winbase.h SYSTEM_POWER_STATUS). Written as local
// constants so the mapping functions stay self-documenting and testable without a
// live power supply.
constexpr unsigned kBatteryFlagHigh = 0x1u;
constexpr unsigned kBatteryFlagLow = 0x2u;
constexpr unsigned kBatteryFlagCritical = 0x4u;
constexpr unsigned kBatteryFlagCharging = 0x8u;
constexpr unsigned kBatteryFlagNoBattery = 0x80u;
constexpr unsigned kBatteryFlagUnknown = 0xFFu;
constexpr unsigned kBatteryPercentUnknown = 255u;
constexpr unsigned kAcLineOnline = 1u;

// GetAdaptersAddresses wants a 15K buffer per Microsoft's own recommendation; the
// retry grows it if the machine has more adapters than that.
constexpr ULONG kAdapterBufferBytes = 16u * 1024u;

// The WLAN client version WlanOpenHandle understands on every supported Windows.
constexpr DWORD kWlanClientVersion = 2u;

std::wstring SsidToWide(const DOT11_SSID& ssid) {
    // An SSID is up to 32 raw octets. Most are ASCII or UTF-8; the conversion is
    // best effort and an undecodable SSID degrades to an empty string rather than
    // to garbage - the popup shows the connection state either way.
    if (ssid.uSSIDLength == 0 || ssid.uSSIDLength > sizeof(ssid.ucSSID)) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        reinterpret_cast<const char*>(ssid.ucSSID),
                                        static_cast<int>(ssid.uSSIDLength), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            reinterpret_cast<const char*>(ssid.ucSSID),
                            static_cast<int>(ssid.uSSIDLength), out.data(), len);
    if (written <= 0) return L"";
    out.resize(static_cast<std::size_t>(written));
    return out;
}

// ---- API readers --------------------------------------------------------------

// One SendMessageTimeoutW to another process's default IME window, with the
// abort-if-hung deadline that keeps a hung foreground application from stalling the
// bar's Tick chain. `out_result` receives the message's return value.
bool ImeControl(HWND ime_window, WPARAM command, LPARAM lparam, DWORD_PTR* out_result) {
    if (ime_window == nullptr || out_result == nullptr) return false;
    *out_result = 0;
    const DWORD sent = SendMessageTimeoutW(ime_window, WM_IME_CONTROL, command, lparam,
                                           SMTO_ABORTIFHUNG, 100, out_result);
    return sent != 0;
}

void SampleIme(HWND tracked_window, StatusSample* out) {
    out->ime_valid = false;
    out->ime_conversion_mode = 0;
    out->ime_native = false;
    if (tracked_window == nullptr) return;
    // ImmGetDefaultIMEWnd answers for another process's window: it returns the
    // default IME class window of the thread that owns `tracked_window`, which is
    // the window WM_IME_CONTROL is documented to be sent to.
    const HWND ime_window = ImmGetDefaultIMEWnd(tracked_window);
    if (ime_window == nullptr) return;
    DWORD_PTR mode = 0;
    if (!ImeControl(ime_window, kImcGetConversionMode, 0, &mode)) return;
    out->ime_valid = true;
    out->ime_conversion_mode = static_cast<unsigned>(mode);
    out->ime_native = (mode & kImeCmodeNative) != 0;
}

void SampleBattery(StatusSample* out) {
    out->battery_present = false;
    out->battery_percent = 0;
    out->battery_charging = false;
    out->battery_ac_online = false;
    SYSTEM_POWER_STATUS sps{};
    if (GetSystemPowerStatus(&sps) == FALSE) return;
    const unsigned flag = sps.BatteryFlag;
    const bool present = BatteryPresentForFlag(flag);
    if (!present) return;
    out->battery_present = true;
    // BatteryLifePercent 255 (unknown) renders as 0 - the same convention
    // Beautiful's UpdateBattery uses (TopBarWindow.xaml.cs:1119), and an unknown
    // charge painted as a healthy 100% would be a lie.
    out->battery_percent = sps.BatteryLifePercent == kBatteryPercentUnknown
                               ? 0
                               : static_cast<int>(sps.BatteryLifePercent);
    out->battery_charging = BatteryChargingFor(sps.ACLineStatus, flag);
    out->battery_ac_online = sps.ACLineStatus == kAcLineOnline;
}

// Whether the machine can currently reach the internet, via the Network List
// Manager. Three answers, not a bool: Unknown is what a refused/absent COM object
// means, and the caller keeps the adapter-gateway answer in that one case - a real
// "router without internet" (NLM says No) must stay No, not be papered over.
enum class NetOnline { Unknown, Yes, No };

NetOnline QueryInternetConnected(struct INetworkListManager** manager, bool* tried) {
    if (*manager == nullptr) {
        if (*tried) return NetOnline::Unknown;
        *tried = true;
        const HRESULT hr = CoCreateInstance(CLSID_NetworkListManager, nullptr,
                                            CLSCTX_INPROC_SERVER, IID_INetworkListManager,
                                            reinterpret_cast<void**>(manager));
        if (FAILED(hr) || *manager == nullptr) {
            DF_DEBUG(kModule, "NetworkListManager unavailable hr=0x%08lX", hr);
            return NetOnline::Unknown;
        }
    }
    VARIANT_BOOL online = VARIANT_FALSE;
    if (FAILED((*manager)->IsConnectedToInternet(&online))) return NetOnline::Unknown;
    return online != VARIANT_FALSE ? NetOnline::Yes : NetOnline::No;
}

// The "best" operational adapter, classified. `kind` is Wifi when any 802.11
// adapter is up with a gateway (the wireless link is what macOS shows while it is
// the active route), Ethernet when a wired one is. Returns true when at least one
// operational adapter with a gateway exists.
bool SampleAdapters(StatusSample* out) {
    out->network_connected = false;
    out->network_kind = ConnectionKind::None;

    std::vector<std::uint8_t> buffer(kAdapterBufferBytes);
    ULONG size = static_cast<ULONG>(buffer.size());
    ULONG rc = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        rc = GetAdaptersAddresses(
            AF_UNSPEC,
            GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &size);
    }
    if (rc != ERROR_SUCCESS) {
        DF_DEBUG(kModule, "GetAdaptersAddresses failed %lu", rc);
        return false;
    }

    bool connected = false;
    bool has_wifi = false;
    bool has_ethernet = false;
    for (const auto* adapter =
             reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        // A gateway is the difference between "the cable is plugged in" and
        // "there is a route": an adapter can be Up while disconnected (media
        // state vs config state), and only the gateway says the link is real.
        if (adapter->FirstGatewayAddress == nullptr) continue;
        connected = true;
        if (adapter->IfType == IF_TYPE_IEEE80211) has_wifi = true;
        if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD) has_ethernet = true;
    }
    out->network_connected = connected;
    out->network_kind = has_wifi ? ConnectionKind::Wifi
                        : has_ethernet ? ConnectionKind::Ethernet
                                       : ConnectionKind::None;
    return connected;
}

// Native WiFi strength for the current association. WlanQueryInterface with
// wlan_intf_opcode_current_connection is a parameter read, not a scan - the scan
// (WlanGetAvailableNetworkList) is the blocking call T-304b's network list will
// need, and it deliberately stays out of the Tick chain here.
void SampleWifi(void* wlan_handle, StatusSample* out) {
    out->wifi_quality = 0;
    out->wifi_bars = 0;
    out->wifi_ssid.clear();
    if (out->network_kind != ConnectionKind::Wifi || wlan_handle == nullptr) return;

    WLAN_INTERFACE_INFO_LIST* list = nullptr;
    if (WlanEnumInterfaces(wlan_handle, nullptr, &list) != ERROR_SUCCESS ||
        list == nullptr) {
        return;
    }
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const GUID& guid = list->InterfaceInfo[i].InterfaceGuid;
        WLAN_CONNECTION_ATTRIBUTES* conn = nullptr;
        DWORD conn_size = 0;
        WLAN_OPCODE_VALUE_TYPE opcode = wlan_opcode_value_type_invalid;
        if (WlanQueryInterface(wlan_handle, &guid, wlan_intf_opcode_current_connection,
                               nullptr, &conn_size,
                               reinterpret_cast<PVOID*>(&conn), &opcode) !=
                ERROR_SUCCESS ||
            conn == nullptr) {
            continue;
        }
        out->wifi_quality = static_cast<int>(conn->wlanAssociationAttributes.wlanSignalQuality);
        out->wifi_bars = WifiBarsForQuality(out->wifi_quality);
        out->wifi_ssid = SsidToWide(conn->wlanAssociationAttributes.dot11Ssid);
        WlanFreeMemory(conn);
        break;  // the first interface that answers is the active association
    }
    WlanFreeMemory(list);
}

}  // namespace

// ---- pure mappings (these define the header declarations) ---------------------

int WifiBarsForQuality(int quality) {
    const int q = std::clamp(quality, 0, 100);
    if (q >= 100) return 4;
    if (q >= 75) return 3;
    if (q >= 50) return 2;
    if (q >= 25) return 1;
    return 0;
}

UsageGrade BatteryGradeForPercent(int percent) {
    // < 20% is the spec's single alarm band (macOS HIG systemRed via the frozen
    // kWidgetCriticalColor). Values below zero are impossible readings; grading
    // them red would punish a broken sensor with an alarm nobody asked for, so
    // they fall through to Normal like the T-302 GradeForPercent degenerate rule.
    if (percent >= 0 && percent < 20) return UsageGrade::Critical;
    return UsageGrade::Normal;
}

bool BatteryPresentForFlag(unsigned battery_flag) {
    return battery_flag != kBatteryFlagUnknown &&
           (battery_flag & kBatteryFlagNoBattery) == 0;
}

bool BatteryChargingFor(unsigned ac_line_status, unsigned battery_flag) {
    return ac_line_status == kAcLineOnline || (battery_flag & kBatteryFlagCharging) != 0;
}

std::wstring ImeConversionLabel(unsigned conversion_mode) {
    return (conversion_mode & kImeCmodeNative) != 0 ? L"\u4E2D" : L"A";  // 中 / A
}

unsigned ImeToggledConversionMode(unsigned conversion_mode) {
    return conversion_mode ^ kImeCmodeNative;
}

const wchar_t* ConnectionKindLabel(ConnectionKind kind) {
    switch (kind) {
        case ConnectionKind::Wifi: return L"Wi-Fi";
        case ConnectionKind::Ethernet: return L"\u4EE5\u592A\u7F51";  // 以太网
        case ConnectionKind::None: break;
    }
    return L"\u65E0\u7F51\u7EDC";  // 无网络
}

// ---- demo hook ----------------------------------------------------------------

bool ApplyStatusDemoIfAny(StatusSample* sample) {
    if (sample == nullptr) return false;
    char mode[16]{};
    const DWORD n = GetEnvironmentVariableA("DOCKFORGE_STATUS_DEMO", mode, sizeof(mode) - 1);
    if (n == 0 || n >= sizeof(mode) - 1) return false;
    if (strcmp(mode, "battery") == 0) {
        sample->battery_present = true;
        sample->battery_percent = 87;
        sample->battery_charging = true;
        sample->battery_ac_online = true;
    } else if (strcmp(mode, "low") == 0) {
        sample->battery_present = true;
        sample->battery_percent = 15;
        sample->battery_charging = false;
        sample->battery_ac_online = false;
    } else if (strcmp(mode, "nobattery") == 0) {
        // Forces the hidden state the DoD wants screenshotted - on a machine that
        // HAS a battery, the real flag can never say 128, so the demo hook is the
        // only deterministic way to photograph "无电池隐藏".
        sample->battery_present = false;
        sample->battery_percent = 0;
        sample->battery_charging = false;
        sample->battery_ac_online = false;
    } else if (strcmp(mode, "wifi4") == 0) {
        sample->network_connected = true;
        sample->network_online = true;
        sample->network_kind = ConnectionKind::Wifi;
        // 4 bars needs the full 100: wlanapi's quality is a linear percentage and
        // the bands are >=100 -> 4, >=75 -> 3 (the same mapping the unit tests pin).
        sample->wifi_quality = 100;
        sample->wifi_bars = WifiBarsForQuality(100);
        sample->wifi_ssid = L"Demo-Ssid";
    } else if (strcmp(mode, "wifi2") == 0) {
        sample->network_connected = true;
        sample->network_online = true;
        sample->network_kind = ConnectionKind::Wifi;
        sample->wifi_quality = 60;
        sample->wifi_bars = WifiBarsForQuality(60);
        sample->wifi_ssid = L"Demo-Ssid";
    } else if (strcmp(mode, "offline") == 0) {
        sample->network_connected = false;
        sample->network_online = false;
        sample->network_kind = ConnectionKind::None;
        sample->wifi_quality = 0;
        sample->wifi_bars = 0;
        sample->wifi_ssid.clear();
    } else {
        DF_WARN(kModule, "DOCKFORGE_STATUS_DEMO=%s is not a known mode; ignored", mode);
        return false;
    }
    DF_WARN(kModule, "DOCKFORGE_STATUS_DEMO=%s active - sampled status replaced", mode);
    return true;
}

// ---- StatusHost ---------------------------------------------------------------

void StatusHost::Start(const core::FinderBarConfig& config) {
    // The host runs when the right table renders at least one of the three status
    // types; a clock-only table (or a bar without the right region) must not pay
    // for sampling, same zero-cost rule as WidgetHost's evidence A/B baseline.
    bool wanted = false;
    for (const core::FinderBarRightItem& entry : config.right) {
        const std::optional<RightType> type =
            RightTypeFromName(core::Utf8ToWide(entry.type).c_str());
        if (!type.has_value() || !RightTypeRegistered(*type)) continue;
        if (*type == RightType::Ime || *type == RightType::Battery ||
            *type == RightType::Network) {
            wanted = true;
            break;
        }
    }
    if (!wanted) {
        if (running_) Stop();
        return;
    }
    if (running_) return;  // the common hot-reload path; the period is fixed
    running_ = true;
    accumulator_ms_ = static_cast<double>(kStatusRefreshMs);  // first sample on first Tick
    DF_INFO(kModule, "status host started (refresh %dms)", kStatusRefreshMs);
}

void StatusHost::Stop() {
    if (!running_) return;
    if (net_list_manager_ != nullptr) {
        net_list_manager_->Release();
        net_list_manager_ = nullptr;
    }
    net_list_manager_tried_ = false;
    if (wlan_handle_ != nullptr) {
        WlanCloseHandle(reinterpret_cast<HANDLE>(wlan_handle_), nullptr);
        wlan_handle_ = nullptr;
    }
    wlan_open_tried_ = false;
    running_ = false;
    has_published_ = false;
    DF_INFO(kModule, "status host stopped");
}

void StatusHost::Sample(HWND tracked_window) {
    // Order matters: the Wi-Fi read keys off the connection kind the adapter pass
    // produces, and the online query may fall back to that pass's answer.
    SampleBattery(&sample_);
    SampleIme(tracked_window, &sample_);
    const bool connected = SampleAdapters(&sample_);
    switch (QueryInternetConnected(&net_list_manager_, &net_list_manager_tried_)) {
        case NetOnline::Yes:
            sample_.network_online = true;
            break;
        case NetOnline::No:
            // NLM answered: a gateway on an adapter without internet is the real
            // "已连接但无互联网" state, and reporting online here would lie.
            sample_.network_online = false;
            break;
        case NetOnline::Unknown:
            // No NetworkListManager answer (COM refused) with a gateway-bearing
            // adapter up: keep the honest "there is a link" instead of flashing
            // 无网络. An adapter answer of not-connected always stands.
            sample_.network_online = connected;
            break;
    }
    if (!wlan_open_tried_) {
        wlan_open_tried_ = true;
        DWORD negotiated = 0;
        const DWORD rc = WlanOpenHandle(kWlanClientVersion, nullptr, &negotiated,
                                        reinterpret_cast<HANDLE*>(&wlan_handle_));
        if (rc != ERROR_SUCCESS) {
            DF_DEBUG(kModule, "WlanOpenHandle failed %lu; no Wi-Fi strength", rc);
            wlan_handle_ = nullptr;
        }
    }
    SampleWifi(wlan_handle_, &sample_);

    // Evidence hook first (it replaces inputs wholesale), then the bookkeeping the
    // 1Hz evidence record diffs against wall clock.
    ApplyStatusDemoIfAny(&sample_);
    ++samples_;
    last_sample_tick_ms_ = GetTickCount64();
}

void StatusHost::SampleNow(HWND tracked_window) {
    if (!running_) return;
    Sample(tracked_window);
    has_published_ = false;  // the next Tick reports the change to the caller
}

bool StatusHost::ToggleImeConversionMode(HWND tracked_window) {
    if (!running_ || tracked_window == nullptr) return false;
    // Re-sample first: the cached mode can be a full second stale, and toggling a
    // stale mode would set the one the user just left.
    Sample(tracked_window);
    has_published_ = false;
    if (!sample_.ime_valid) return false;
    const HWND ime_window = ImmGetDefaultIMEWnd(tracked_window);
    if (ime_window == nullptr) return false;
    const unsigned target = ImeToggledConversionMode(sample_.ime_conversion_mode);
    DWORD_PTR result = 0;
    const bool sent = ImeControl(ime_window, kImcSetConversionMode,
                                 static_cast<LPARAM>(target), &result);
    DF_INFO(kModule, "ime toggle mode 0x%X -> 0x%X sent=%d", sample_.ime_conversion_mode,
            target, sent ? 1 : 0);
    // Read back immediately so the bar's label flips in the same frame - the
    // "即时切换" the DoD names. The next 1s sample would converge anyway, but a
    // click that updates one second later reads as broken.
    Sample(tracked_window);
    return sent;
}

bool StatusHost::Tick(double delta_ms, HWND tracked_window) {
    if (!running_) return false;
    accumulator_ms_ += std::max(0.0, delta_ms);
    if (accumulator_ms_ < static_cast<double>(kStatusRefreshMs)) return false;
    accumulator_ms_ = 0.0;

    Sample(tracked_window);

    // Bar-visible fields only: the raw conversion mode can change while its label
    // (and therefore the bar) does not, and the SSID never touches the bar. A
    // churn-free comparison is what keeps the right region from re-laying out
    // every second for cosmetic changes.
    bool changed = !has_published_;
    if (has_published_) {
        changed = sample_.ime_valid != published_.ime_valid ||
                  sample_.ime_native != published_.ime_native ||
                  sample_.battery_present != published_.battery_present ||
                  sample_.battery_percent != published_.battery_percent ||
                  sample_.battery_charging != published_.battery_charging ||
                  sample_.network_connected != published_.network_connected ||
                  sample_.network_online != published_.network_online ||
                  sample_.network_kind != published_.network_kind ||
                  sample_.wifi_bars != published_.wifi_bars;
    }
    published_ = sample_;
    has_published_ = true;
    return changed;
}

std::string StatusHost::SnapshotJson() const {
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer),
                  "{\"samples\":%llu,\"last_sample_tick_ms\":%llu,\"refresh_ms\":%d,"
                  "\"ime\":{\"valid\":%s,\"mode\":%u,\"label\":\"%s\"},"
                  "\"battery\":{\"present\":%s,\"percent\":%d,\"charging\":%s,"
                  "\"ac_online\":%s,\"grade\":\"%s\"},"
                  "\"network\":{\"connected\":%s,\"online\":%s,\"kind\":\"%s\","
                  "\"quality\":%d,\"bars\":%d}}",
                  static_cast<unsigned long long>(samples_),
                  static_cast<unsigned long long>(last_sample_tick_ms_), kStatusRefreshMs,
                  sample_.ime_valid ? "true" : "false", sample_.ime_conversion_mode,
                  core::WideToUtf8(ImeConversionLabel(sample_.ime_conversion_mode)).c_str(),
                  sample_.battery_present ? "true" : "false", sample_.battery_percent,
                  sample_.battery_charging ? "true" : "false",
                  sample_.battery_ac_online ? "true" : "false",
                  GradeName(BatteryGradeForPercent(sample_.battery_percent)),
                  sample_.network_connected ? "true" : "false",
                  sample_.network_online ? "true" : "false",
                  core::WideToUtf8(ConnectionKindLabel(sample_.network_kind)).c_str(),
                  sample_.wifi_quality, sample_.wifi_bars);
    return buffer;
}

}  // namespace dockforge::finderbar
