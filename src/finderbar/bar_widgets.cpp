// DockForge - Finder Bar centre widget zone implementation (T-302).
#include "finderbar/bar_widgets.h"

#include "core/log.h"
#include "core/paths.h"

// MinGW include-order shim for the MIB2 APIs. iphlpapi.h includes netioapi.h
// with __IPHLPAPI_H__ already set, so netioapi.h's own else-branch (which would
// pull ws2def/ws2ipdef/ifdef/nldef in) is skipped, and the MIB2 block it guards
// with _WS2IPDEF_ goes undeclared. win_common.h compiles with
// WIN32_LEAN_AND_MEAN, so windows.h brought no winsock either. The four headers
// below supply exactly what netioapi.h then still needs:
//   winsock2.h   -> ADDRESS_FAMILY (typedef u_short)
//   ws2ipdef.h   -> _WS2IPDEF_ (the guard over the MIB2 block) + sockaddr_in6
//   ifdef.h      -> NET_IFINDEX / NET_LUID
//   nldef.h      -> the NET_LUID union ifdef.h consumes
#include <winsock2.h>
#include <ws2ipdef.h>
#include <nldef.h>
#include <ifdef.h>
#include <iphlpapi.h>
#include <oleauto.h>
#include <wbemidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdlib>

namespace dockforge::finderbar {
namespace {

constexpr char kModule[] = "finderbar";

}  // namespace

// ---- type registry (these define the bar_widgets.h declarations) --------------

const wchar_t* WidgetTypeName(int type_index) {
    // C-31's declaration sequence for finderbar.widgets.
    static const wchar_t* kNames[kWidgetTypeCount] = {
        L"cpu", L"memory", L"disk", L"network",
        L"temperature", L"weather", L"media", L"calendar_brief",
    };
    if (type_index < 0 || type_index >= kWidgetTypeCount) return L"";
    return kNames[type_index];
}

std::optional<WidgetType> WidgetTypeFromName(const wchar_t* name) {
    if (name == nullptr) return std::nullopt;
    for (int i = 0; i < kWidgetTypeCount; ++i) {
        if (wcscmp(name, WidgetTypeName(i)) == 0) return static_cast<WidgetType>(i);
    }
    return std::nullopt;
}

bool WidgetTypeRegistered(WidgetType type) {
    // T-302 (this package) registers the five monitoring widgets. weather /
    // media / calendar_brief stay unrenderable until T-302d flips them here -
    // an unregistered type is skipped, never drawn as an empty placeholder.
    switch (type) {
        case WidgetType::Cpu:
        case WidgetType::Memory:
        case WidgetType::Disk:
        case WidgetType::Network:
        case WidgetType::Temperature:
            return true;
        default:
            return false;
    }
}

// ---- grade -------------------------------------------------------------------

UsageGrade GradeForPercent(float percent) {
    // The frozen thresholds: 80 and 90. Boundary values belong to the band they
    // close (80 is warn's lower edge, 90 is warn's upper edge: the spec grades
    // "80-90" orange and ">90" red). NaN compares false on every branch below
    // and therefore lands on Normal, which is what an impossible reading gets.
    if (percent >= 80.0f && percent <= 90.0f) return UsageGrade::Warn;
    if (percent > 90.0f) return UsageGrade::Critical;
    return UsageGrade::Normal;
}

render::ColorF WidgetGradeColor(UsageGrade grade) {
    switch (grade) {
        case UsageGrade::Warn:
            return kWidgetWarnColor;      // HIG systemOrange #FF9F0A
        case UsageGrade::Critical:
            return kWidgetCriticalColor;  // HIG systemRed #FF453A
        case UsageGrade::Normal:
            break;                        // the theme foreground; the caller picks it
    }
    return render::ColorF{1.0f, 1.0f, 1.0f, 0.0f};
}

const char* GradeName(UsageGrade grade) {
    switch (grade) {
        case UsageGrade::Warn: return "warn";
        case UsageGrade::Critical: return "critical";
        case UsageGrade::Normal: break;
    }
    return "normal";
}

// ---- pure helpers ------------------------------------------------------------

float ComputeDiskPercent(std::uint64_t used_bytes, std::uint64_t total_bytes) {
    if (total_bytes == 0) return 0.0f;
    const double ratio = static_cast<double>(used_bytes) / static_cast<double>(total_bytes);
    if (ratio <= 0.0) return 0.0f;
    if (ratio >= 1.0) return 100.0f;
    return static_cast<float>(ratio * 100.0);
}

double ComputeNetworkRate(std::uint64_t prev_octets, std::uint64_t cur_octets,
                          double elapsed_seconds) {
    if (elapsed_seconds <= 0.0) return 0.0;
    // Octets reset to zero when an adapter bounces. A backwards counter is a
    // reset, not a negative rate: report 0 for that interval and resync next time.
    if (cur_octets < prev_octets) return 0.0;
    return static_cast<double>(cur_octets - prev_octets) / elapsed_seconds;
}

std::wstring WidgetDisplayText(WidgetType type, const WidgetSnapshot& snapshot) {
    wchar_t buffer[64]{};
    switch (type) {
        case WidgetType::Cpu:
            swprintf_s(buffer, L"CPU %.0f%%", snapshot.cpu_percent);
            return buffer;
        case WidgetType::Memory:
            swprintf_s(buffer, L"MEM %.0f%%", snapshot.memory_percent);
            return buffer;
        case WidgetType::Disk:
            swprintf_s(buffer, L"DISK %.0f%%", snapshot.disk_percent);
            return buffer;
        case WidgetType::Network: {
            // iStat-style rate formatting: one decimal in MB/s once past a
            // megabyte, whole KB/s below it. 0 stays "0KB/s", not blank - a
            // blank would be indistinguishable from a broken sampler.
            const double rate = snapshot.network_bytes_per_sec;
            if (rate >= 1000000.0) {
                swprintf_s(buffer, L"NET %.1fMB/s", rate / 1000000.0);
            } else {
                swprintf_s(buffer, L"NET %.0fKB/s", rate / 1000.0);
            }
            return buffer;
        }
        case WidgetType::Temperature:
            // No usable sensor means no text, and BuildWidgetItems drops empty
            // entries: an unavailable temperature renders as NOTHING, never as a
            // fabricated number (DoD: "不可用则该组件不显示且不崩溃").
            if (!snapshot.temperature_valid) return {};
            swprintf_s(buffer, L"TEMP %.0f\u00B0C", snapshot.temperature_c);
            return buffer;
        default:
            return {};  // T-302d types render nothing this package
    }
}

namespace {

// The grade a given widget renders at, from one snapshot. Documented in the
// header: percentages grade by the frozen thresholds, temperature grades by the
// same numbers read as degrees Celsius, network always stays Normal.
UsageGrade WidgetGrade(WidgetType type, const WidgetSnapshot& snapshot) {
    switch (type) {
        case WidgetType::Cpu: return GradeForPercent(snapshot.cpu_percent);
        case WidgetType::Memory: return GradeForPercent(snapshot.memory_percent);
        case WidgetType::Disk: return GradeForPercent(snapshot.disk_percent);
        case WidgetType::Temperature:
            return snapshot.temperature_valid ? GradeForPercent(snapshot.temperature_c)
                                              : UsageGrade::Normal;
        case WidgetType::Network:
        default:
            return UsageGrade::Normal;
    }
}

// ---- light API readers (microsecond scale) -----------------------------------

// The system drive, i.e. the volume Windows itself lives on. MyDockFinder / iStat
// class tools report the boot volume's fill level, and that is the reading with
// one unambiguous meaning; enumerating every volume would need a second row.
std::wstring SystemDriveRoot() {
    wchar_t dir[MAX_PATH]{};
    const UINT n = GetSystemWindowsDirectoryW(dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH && dir[1] == L':') {
        std::wstring root(dir, 2);  // "C:"
        root += L"\\";
        return root;
    }
    return L"C:\\";  // the boot drive by definition, when the query is refused
}

// Sum of RX+TX octets over every interface that is up and not loopback. Loopback
// alone can carry more octets than all real traffic combined, so counting it
// would make the number meaningless.
std::uint64_t ReadNetworkOctets() {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR || table == nullptr) return 0;
    std::uint64_t sum = 0;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IF_ROW2& row = table->Table[i];
        if (row.OperStatus != IfOperStatusUp) continue;
        if (row.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        sum += row.InOctets + row.OutOctets;
    }
    FreeMibTable(table);
    return sum;
}

// ---- thermal worker (WMI, its own thread) ------------------------------------

// MSAcpi_ThermalZoneTemperature.CurrentTemperature is tenths of a kelvin
// (Beautiful Core/Thermal/TemperatureReader.cs KelvinTenthsToCelsius documents
// the same unit). -> milli-degrees Celsius for the atomic slot.
constexpr std::int32_t KelvinTenthsToMilliC(std::int32_t tenths_kelvin) {
    // (tenths / 10 - 273.15) * 1000
    return static_cast<std::int32_t>(std::lround(tenths_kelvin * 100.0) - 273150);
}

// Milliseconds between WMI reconnect attempts after a failure. A healthy query
// costs single-digit milliseconds; a refused connection costs more, so failures
// back off hard instead of hammering the WMI service every 2 seconds.
constexpr int kThermalRetryMs = 15000;

struct ThermalRuntime {
    std::atomic<bool>* stop = nullptr;
    std::atomic<std::int32_t>* out_milli_c = nullptr;
    std::atomic<int>* refresh_ms = nullptr;
};

// Releases in dependency order and clears the pointer.
template <typename T>
void SafeRelease(T** ptr) {
    if (ptr != nullptr && *ptr != nullptr) {
        (*ptr)->Release();
        *ptr = nullptr;
    }
}

// One WMI round trip on an established connection. Returns the first
// CurrentTemperature the zone exposes, or false when the query fails or the
// reading is absurd (a thermal zone reporting < -50C or > 200C is broken, and a
// broken sensor must not paint a number).
bool QueryThermalOnce(IWbemServices* services, std::int32_t* out_milli_c) {
    IEnumWbemClassObject* rows = nullptr;
    BSTR wql = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT CurrentTemperature, InstanceName FROM MSAcpi_ThermalZoneTemperature");
    const auto free_strings = MakeGuard([&] {
        SysFreeString(wql);
        SysFreeString(query);
    });
    if (wql == nullptr || query == nullptr || services == nullptr) return false;

    const HRESULT hr = services->ExecQuery(wql, query,
                                           WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                           nullptr, &rows);
    if (FAILED(hr) || rows == nullptr) return false;

    bool ok = false;
    IWbemClassObject* row = nullptr;
    ULONG returned = 0;
    // 2s server-side wait cap: a hung WMI provider must not stall this thread
    // forever - the atomic simply keeps its previous value and TryRefresh
    // reports failure, which is the "component hidden" path.
    if (SUCCEEDED(rows->Next(2000, 1, &row, &returned)) && returned == 1 && row != nullptr) {
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(row->Get(L"CurrentTemperature", 0, &value, nullptr, nullptr)) &&
            (value.vt == VT_I4 || value.vt == VT_UI4)) {
            const std::int32_t tenths = value.vt == VT_I4
                                            ? value.lVal
                                            : static_cast<std::int32_t>(value.ulVal);
            const std::int32_t milli_c = KelvinTenthsToMilliC(tenths);
            // -50C .. 200C is the plausible envelope for an ACPI zone reading.
            if (milli_c > -50000 && milli_c < 200000) {
                *out_milli_c = milli_c;
                ok = true;
            }
        }
        VariantClear(&value);
        row->Release();
    }
    rows->Release();
    return ok;
}

DWORD WINAPI ThermalThreadMain(LPVOID param) {
    auto* runtime = static_cast<ThermalRuntime*>(param);

    // This thread owns its own COM apartment; it never touches any window or
    // surface, only the atomic slot, so no main-thread synchronisation exists.
    const HRESULT hr_init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_ok = SUCCEEDED(hr_init);

    IWbemLocator* locator = nullptr;
    IWbemServices* services = nullptr;

    while (!runtime->stop->load(std::memory_order_relaxed)) {
        bool success = false;
        std::int32_t reading = 0;
        if (com_ok && services == nullptr) {
            // (Re)connect. The locator is cheap enough to build per attempt and
            // released with the thread, so a failed connect leaves no half-open
            // proxy behind.
            if (locator == nullptr) {
                CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_IWbemLocator, reinterpret_cast<void**>(&locator));
            }
            if (locator != nullptr) {
                BSTR ns = SysAllocString(L"ROOT\\WMI");
                const HRESULT hr = locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0,
                                                          nullptr, nullptr, &services);
                SysFreeString(ns);
                if (SUCCEEDED(hr) && services != nullptr) {
                    // Local WMI wants the impersonation blanket on the proxy;
                    // without it ExecQuery fails with E_ACCESSDENIED on some
                    // configurations. Failure here just falls through to retry.
                    CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                                      nullptr, EOAC_NONE);
                } else {
                    SafeRelease(&services);
                }
            }
        }
        if (services != nullptr) {
            success = QueryThermalOnce(services, &reading);
        }

        if (success) {
            runtime->out_milli_c->store(reading, std::memory_order_relaxed);
        } else {
            // No usable sensor this round: publish the invalid sentinel so the
            // widget disappears rather than showing a stale or fake value.
            runtime->out_milli_c->store(WidgetHost::kTemperatureInvalid,
                                        std::memory_order_relaxed);
            // Log the failure ONCE, not every retry: the widget renders nothing,
            // and an unavailable sensor is a fact worth one log line, not a
            // fifteen-second heartbeat.
            static std::atomic<bool> logged_once{false};
            bool expected = false;
            if (logged_once.compare_exchange_strong(expected, true)) {
                DF_WARN(kModule,
                        "WMI temperature unavailable (MSAcpi_ThermalZoneTemperature); "
                        "the temperature widget renders nothing on this machine");
            }
            if (services != nullptr) {
                // A query that fails on a live connection is usually a driver or
                // permission change; drop the proxy so the next round reconnects
                // from scratch.
                SafeRelease(&services);
            }
        }

        // Sleep in 100ms slices so Stop() never waits a full period.
        const int period =
            success ? runtime->refresh_ms->load(std::memory_order_relaxed) : kThermalRetryMs;
        for (int waited = 0; waited < period && !runtime->stop->load(std::memory_order_relaxed);
             waited += 100) {
            Sleep(100);
        }
    }

    SafeRelease(&services);
    SafeRelease(&locator);
    if (com_ok) CoUninitialize();
    return 0;
}

// ---- evidence demo mode ------------------------------------------------------

// DOCKFORGE_WIDGETS_DEMO=<normal|warn|critical>: replaces sampled values with
// fixed ones so the three grade tiers can be screenshotted deterministically.
// Same precedent as DOCKFORGE_FORCE_MATERIAL / DOCKFORGE_WEATHER_API_HOST: the
// hook replaces an input, never the rendering or layout path.
struct DemoValues {
    float cpu = 0.0f;
    float memory = 0.0f;
    float disk = 0.0f;
    double network_bps = 0.0;
    float temperature = 0.0f;
    bool temperature_valid = false;
};

bool ParseDemoMode(const char* mode, DemoValues* out) {
    if (mode == nullptr || out == nullptr) return false;
    if (strcmp(mode, "normal") == 0) {
        *out = DemoValues{42.0f, 63.0f, 55.0f, 375000.0, 45.0f, true};
    } else if (strcmp(mode, "warn") == 0) {
        *out = DemoValues{85.0f, 82.0f, 88.0f, 2400000.0, 85.0f, true};
    } else if (strcmp(mode, "critical") == 0) {
        *out = DemoValues{95.0f, 93.0f, 96.0f, 12800000.0, 95.0f, true};
    } else {
        return false;
    }
    return true;
}

}  // namespace

// ---- BuildWidgetItems / LayoutWidgetGroup ------------------------------------

std::vector<WidgetItem> BuildWidgetItems(const core::FinderBarConfig& config,
                                         const WidgetSnapshot& snapshot) {
    std::vector<WidgetItem> items;
    items.reserve(config.widgets.size());
    for (const core::FinderBarWidgetItem& entry : config.widgets) {
        // The config stores the type as UTF-8 (C-01 JSON round-trip); the layout
        // speaks wide strings. Unknown type strings stay in the config file but
        // produce no box, and the skip is logged once per relayout, not per frame.
        const std::wstring type_name = core::Utf8ToWide(entry.type);
        const std::optional<WidgetType> type = WidgetTypeFromName(type_name.c_str());
        if (!type.has_value()) {
            DF_DEBUG(kModule, "finderbar.widgets type '%s' is not known, skipped",
                     entry.type.c_str());
            continue;
        }
        if (!WidgetTypeRegistered(*type)) continue;  // T-302d lands here
        WidgetItem item;
        item.type = *type;
        item.order = entry.order;
        item.text = WidgetDisplayText(*type, snapshot);
        // An empty label (an unavailable sensor) renders nothing: no box, no gap.
        if (item.text.empty()) continue;
        item.grade = WidgetGrade(*type, snapshot);
        items.push_back(std::move(item));
    }
    return items;
}

namespace {

float WidgetItemWidth(const WidgetItem& item, MeasureFn measure) {
    const float text_width = measure(item.text.c_str(), kWidgetFontDip);
    return kWidgetContentPaddingDip * 2.0f + text_width;
}

}  // namespace

std::vector<WidgetBox> LayoutWidgetGroup(const std::vector<WidgetItem>& items,
                                         float height_dip, float left_limit_dip,
                                         float right_limit_dip, MeasureFn measure,
                                         WidgetGroupSummary* summary) {
    WidgetGroupSummary local;
    const MeasureFn measure_fn = measure != nullptr ? measure : &ApproximateTextWidth;
    const float bar_height = height_dip > 0.0f ? height_dip : kDefaultBarHeightDip;
    const float left_limit = std::max(0.0f, left_limit_dip);
    const float right_limit = std::max(left_limit, right_limit_dip);

    // Stable sort by (order, type): order decides the position from the left, the
    // C-31 declaration sequence breaks ties, stability keeps equal pairs in the
    // caller's sequence - the render is a function of the file, not of qsort's mood.
    std::vector<const WidgetItem*> sorted;
    sorted.reserve(items.size());
    for (const WidgetItem& item : items) sorted.push_back(&item);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const WidgetItem* a, const WidgetItem* b) {
                         if (a->order != b->order) return a->order < b->order;
                         return static_cast<int>(a->type) < static_cast<int>(b->type);
                     });

    // Total run width including the gaps, so the row can be anchored to the
    // centre of the free span in one pass.
    float total = 0.0f;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        total += WidgetItemWidth(*sorted[i], measure_fn);
        if (i + 1 < sorted.size()) total += kWidgetGapDip;
    }

    const float span = std::max(0.0f, right_limit - left_limit);
    const float centre = left_limit + span * 0.5f;
    // Centre-anchored when the row fits; clamped to the left limit when it does
    // not (right_limit - total < left_limit then, and the clamp collapses there).
    const float anchor = std::clamp(centre - total * 0.5f, left_limit,
                                    std::max(left_limit, right_limit - total));
    local.start_dip = anchor;
    local.total_width_dip = total;

    float cursor = anchor;
    std::vector<WidgetBox> out;
    out.reserve(sorted.size());
    for (const WidgetItem* item : sorted) {
        const float box_width = WidgetItemWidth(*item, measure_fn);
        // An entry that would cross the right limit is dropped, not squeezed: a
        // clipped label or a box under the right region is a visible bug, and the
        // summary reports the drop instead of hiding it.
        if (cursor + box_width > right_limit) {
            ++local.dropped;
            continue;
        }
        WidgetBox box;
        box.rect = render::RectF{cursor, 0.0f, cursor + box_width, bar_height};
        box.type = item->type;
        box.text = item->text;
        box.grade = item->grade;
        out.push_back(box);
        cursor += box_width + kWidgetGapDip;
    }

    // Shrink the reported width to what was actually placed: a drop must not
    // leave the summary claiming more space than the row occupies.
    if (out.empty()) {
        local.total_width_dip = 0.0f;
    } else {
        const float last_right = out.back().rect.right;
        local.total_width_dip = std::max(0.0f, last_right - out.front().rect.left);
    }
    local.placed = out.size();
    if (summary != nullptr) *summary = local;
    return out;
}

// ---- WidgetHost --------------------------------------------------------------

bool AnyRegisteredWidget(const std::vector<core::FinderBarWidgetItem>& widgets) {
    for (const core::FinderBarWidgetItem& entry : widgets) {
        const std::optional<WidgetType> type =
            WidgetTypeFromName(core::Utf8ToWide(entry.type).c_str());
        if (type.has_value() && WidgetTypeRegistered(*type)) return true;
    }
    return false;
}

WidgetHost::~WidgetHost() { Stop(); }

void WidgetHost::Start(const core::FinderBarConfig& config) {
    if (running_) return;
    refresh_ms_ = std::max(2000, config.widgets_refresh_ms);

    any_registered_ = AnyRegisteredWidget(config.widgets);
    // Nothing configured that we render -> nothing to run. The A/B baseline in
    // evidence/T-302 relies on this being a true zero.
    if (!any_registered_) return;

    running_ = true;
    thermal_stop_.store(false, std::memory_order_relaxed);
    thermal_refresh_ms_.store(refresh_ms_, std::memory_order_relaxed);
    auto* runtime = new ThermalRuntime{&thermal_stop_, &thermal_milli_c_, &thermal_refresh_ms_};
    thermal_thread_ = std::thread([runtime] {
        ThermalThreadMain(reinterpret_cast<LPVOID>(runtime));
        delete runtime;
    });
    DF_INFO(kModule, "widget host started: %zu entries, refresh %dms (thermal on its own thread)",
            config.widgets.size(), refresh_ms_);
}

void WidgetHost::Stop() {
    if (!running_) return;
    thermal_stop_.store(true, std::memory_order_relaxed);
    if (thermal_thread_.joinable()) thermal_thread_.join();
    running_ = false;
    DF_INFO(kModule, "widget host stopped");
}

bool WidgetHost::Tick(double delta_ms, const core::FinderBarConfig& config) {
    if (!running_) return false;
    // A config reload can change the period mid-run; the thermal thread learns it
    // through the same atomic the next time it wakes up.
    if (config.widgets_refresh_ms != refresh_ms_) {
        refresh_ms_ = std::max(2000, config.widgets_refresh_ms);
        thermal_refresh_ms_.store(refresh_ms_, std::memory_order_relaxed);
    }

    accumulator_ms_ += std::max(0.0, delta_ms);
    if (accumulator_ms_ < static_cast<double>(refresh_ms_)) return false;
    accumulator_ms_ = 0.0;

    SampleLight();

    // Publish the thermal slot: one atomic load, no lock. Invalid stays invalid.
    // This runs BEFORE the demo override so DOCKFORGE_WIDGETS_DEMO can present a
    // usable temperature on a machine whose ACPI zone is access-denied - the
    // evidence capture needs the widget visible to screenshot its colour tier.
    const std::int32_t milli_c = thermal_milli_c_.load(std::memory_order_relaxed);
    if (milli_c != WidgetHost::kTemperatureInvalid) {
        snapshot_.temperature_valid = true;
        snapshot_.temperature_c = static_cast<float>(milli_c) / 1000.0f;
    } else {
        snapshot_.temperature_valid = false;
        snapshot_.temperature_c = 0.0f;
    }

    ApplyDemoIfAny();

    // Text-level change detection: the layout only churns when a rendered label
    // or a grade actually moved, not on every sub-percent fluctuation.
    bool changed = !has_published_;
    if (has_published_) {
        for (int i = 0; i < kWidgetTypeCount; ++i) {
            const auto type = static_cast<WidgetType>(i);
            if (!WidgetTypeRegistered(type)) continue;
            const std::wstring before = WidgetDisplayText(type, published_);
            const std::wstring after = WidgetDisplayText(type, snapshot_);
            if (before != after || WidgetGrade(type, published_) != WidgetGrade(type, snapshot_)) {
                changed = true;
                break;
            }
        }
    }
    published_ = snapshot_;
    has_published_ = true;
    return changed;
}

void WidgetHost::SampleLight() {
    // CPU + memory: the exact reader functions system_stats.cpp uses for the C-23
    // event (GetSystemTimes + GlobalMemoryStatusEx). Microsecond scale.
    const SystemCounters cur = ReadSystemCounters();
    snapshot_.cpu_percent =
        has_prev_counters_ ? ComputeCpuPercent(prev_counters_, cur) : 0.0f;
    snapshot_.memory_percent =
        ComputeMemoryPercent(cur.memory_used_bytes, cur.memory_total_bytes);
    prev_counters_ = cur;
    has_prev_counters_ = true;

    // Disk: the boot volume's fill level via GetDiskFreeSpaceExW.
    const std::wstring root = SystemDriveRoot();
    ULARGE_INTEGER total{};
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, &free_bytes)) {
        const std::uint64_t used =
            total.QuadPart > free_bytes.QuadPart
                ? total.QuadPart - free_bytes.QuadPart
                : 0;
        snapshot_.disk_percent = ComputeDiskPercent(used, total.QuadPart);
    }

    // Network: octet sums across up, non-loopback interfaces, differenced against
    // the previous sample's wall clock. GetTickCount64 never wraps and never
    // skips backwards, so the elapsed time is always honest.
    const std::uint64_t octets = ReadNetworkOctets();
    const std::uint64_t now = GetTickCount64();
    if (has_prev_net_) {
        const double elapsed_s =
            static_cast<double>(now - prev_net_tick_ms_) / 1000.0;
        snapshot_.network_bytes_per_sec =
            ComputeNetworkRate(prev_octets_, octets, elapsed_s);
    }
    prev_octets_ = octets;
    prev_net_tick_ms_ = now;
    has_prev_net_ = true;
}

void WidgetHost::ApplyDemoIfAny() {
    char mode[16]{};
    const DWORD n = GetEnvironmentVariableA("DOCKFORGE_WIDGETS_DEMO", mode, sizeof(mode) - 1);
    if (n == 0 || n >= sizeof(mode) - 1) return;
    DemoValues values{};
    if (!ParseDemoMode(mode, &values)) return;
    snapshot_.cpu_percent = values.cpu;
    snapshot_.memory_percent = values.memory;
    snapshot_.disk_percent = values.disk;
    snapshot_.network_bytes_per_sec = values.network_bps;
    snapshot_.temperature_valid = values.temperature_valid;
    snapshot_.temperature_c = values.temperature;
}

std::string WidgetHost::SnapshotJson() const {
    char buffer[256]{};
    const int written = std::snprintf(
        buffer, sizeof(buffer),
        "{\"cpu_percent\":%.2f,\"memory_percent\":%.2f,\"disk_percent\":%.2f,"
        "\"network_bps\":%.1f,\"temperature_valid\":%s,\"temperature_c\":%.1f}",
        static_cast<double>(snapshot_.cpu_percent), static_cast<double>(snapshot_.memory_percent),
        static_cast<double>(snapshot_.disk_percent), snapshot_.network_bytes_per_sec,
        snapshot_.temperature_valid ? "true" : "false",
        static_cast<double>(snapshot_.temperature_c));
    return written > 0 ? std::string(buffer) : std::string("{}");
}

}  // namespace dockforge::finderbar
