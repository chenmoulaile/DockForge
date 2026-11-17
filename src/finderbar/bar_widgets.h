// DockForge - Finder Bar centre widget zone (T-302 + T-302d, CONTRACTS.md C-31).
//
// The centre region renders the widget family C-31 enumerates for
// `finderbar.widgets`: the five monitoring widgets (cpu, memory, disk, network,
// temperature, T-302) plus weather, media and calendar_brief (T-302d). Everything
// that decides what a widget LOOKS like is a pure function over numbers and lives
// here so the unit tests can pin it - the same split that makes bar_layout.h
// testable: geometry and text are pure, the window stays in finder_bar.cpp.
//
// DATA CHANNEL RULING (recorded in DECISIONS.md ADR-0054, and the reason this file
// does not touch C-23):
//
//   * C-23 (P1.5 frozen) SystemStatsUpdated carries exactly cpu_percent,
//     memory_percent, memory_used_bytes, memory_total_bytes, is sampled at
//     <=0.5Hz, and forbids sampling on the render path. That payload is also only
//     broadcast while the hardware panel is OPEN (hardware_panel.h:47 - "Open()
//     // show + start ticking"): the panel's sampler runs behind its own 2s
//     accumulator and stops when the panel hides. The bar's widgets need a
//     PERSISTENT data stream, which C-23's panel-scoped event cannot provide.
//     Extending the frozen payload would be a contract change for no gain, so it
//     is not done - the widgets self-sample instead.
//
//   * The four light quantities (CPU, memory, disk, network) are sampled on the
//     bar's existing Tick chain (FinderBar::Tick -> WidgetHost::Tick) at the
//     configurable period (default 2000ms, C-23's 0.5Hz). Every read is a
//     microsecond-scale API call (GetSystemTimes via system_stats' own reader,
//     GlobalMemoryStatusEx, GetDiskFreeSpaceExW, GetIfTable2) and none of them
//     runs inside Paint - the C-23 prohibition targets the render path, and the
//     Tick chain is where the panel's own sampler already lives.
//
//   * Temperature is the one WMI query (MSAcpi_ThermalZoneTemperature), and WMI
//     can block for tens of milliseconds, so it never touches the Tick chain:
//     a dedicated std::thread polls it and publishes an atomic millikelvin-
//     -scale snapshot that rendering reads lock-free. WinRing0 is forbidden
//     here (T-206 DoD names it explicitly); WMI is the only source. When WMI
//     cannot answer - driver missing, zone not exposed - the temperature widget
//     renders NOTHING rather than a made-up number (same rule as Beautiful's
//     thermal chip: "没有任何数据源时芯片整体隐藏", AppSettings.cs:354).
#pragma once

#include "core/config.h"
#include "dock/weather.h"
#include "finderbar/bar_layout.h"
#include "finderbar/system_stats.h"
#include "render/backend.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace dockforge::finderbar {

// C-31's declaration sequence for finderbar.widgets, verbatim. All eight types
// have renderers since T-302d: the five monitoring widgets from T-302, plus
// weather (dock weather snapshot chain), media (read-only GSMTC display) and
// calendar_brief (month/day). (RightTypeRegistered is the right-region
// precedent - a type whose task has not landed costs no box, no gap.)
enum class WidgetType {
    Cpu = 0,
    Memory,
    Disk,
    Network,
    Temperature,
    Weather,        // T-302d: dock::DrawWeatherGlyph + temperature, placeholder "--°"
    Media,          // T-302d: read-only GSMTC title + play state (controls are T-304b)
    CalendarBrief,  // T-302d: month/day
};

constexpr int kWidgetTypeCount = 8;

// The C-31 spelling of each type ("cpu", "weather", ...). Declaration order.
const wchar_t* WidgetTypeName(int type_index);
std::optional<WidgetType> WidgetTypeFromName(const wchar_t* name);

// True when a renderer exists for this type in the current build. All eight are
// registered since T-302d; the function stays because "is this type rendered"
// is a decision, not an enum property - a future build may unregister one.
bool WidgetTypeRegistered(WidgetType type);

// ---------------------------------------------------------------------------
// The three-tier usage grade (T-302 spec, frozen constants).
//
//   < 80%  -> Normal   : the theme's foreground colour (white on the dark plate)
//   80-90% -> Warn     : macOS HIG systemOrange #FF9F0A
//   > 90%  -> Critical : macOS HIG systemRed #FF453A
//
// The two hex values are the HIG standard values, NOT the Beautiful palette:
// Beautiful MacOS.xaml defines no orange/red alert keys at all (grep 9F0A / 453A /
// Orange / Red = zero hits, verified for this task), so there is nothing to copy
// from there and the HIG values are the documented source.
//
// Temperature reuses the same three tiers with the same numbers read as degrees
// Celsius (<80 / 80-90 / >90), because a temperature widget graded on a
// percentage of some "critical temperature" would need a value ACPI does not
// reliably expose. Network is a rate, not a fraction, so it always renders in
// the theme colour - a grade on "bytes per second" would be a made-up scale.
// ---------------------------------------------------------------------------
enum class UsageGrade { Normal = 0, Warn = 1, Critical = 2 };

// The mapping. Independent and unit tested. Degenerate inputs (negative, NaN)
// grade Normal: an impossible reading must never paint an alarm colour.
UsageGrade GradeForPercent(float percent);

// macOS HIG systemOrange, #FF9F0A (dark-mode value; the bar's plate is a fixed
// dark tint, so one value serves both themes - see render/material.cpp).
constexpr render::ColorF kWidgetWarnColor{1.0f, 159.0f / 255.0f, 10.0f / 255.0f, 1.0f};
// macOS HIG systemRed, #FF453A.
constexpr render::ColorF kWidgetCriticalColor{1.0f, 69.0f / 255.0f, 58.0f / 255.0f, 1.0f};

// The colour a box renders in: the theme foreground for Normal, the frozen HIG
// constants otherwise.
render::ColorF WidgetGradeColor(UsageGrade grade);
const char* GradeName(UsageGrade grade);  // "normal" / "warn" / "critical" (probe JSON)

// ---------------------------------------------------------------------------
// One sample of everything the widgets show. Percentages are 0..100; the network
// rate is bytes per second across all up, non-loopback interfaces.
//
// T-302d additions: the weather scalars mirror the dock snapshot chain the bar
// receives (FinderBar::SetWeatherSnapshot forwards them here); the media scalars
// are the read-only GSMTC poll answer (title kept whole here, truncated only at
// display time); calendar_month/day feed the brief label. All of them live in
// the same struct so WidgetDisplayText and the change detection stay pure
// functions over one snapshot.
// ---------------------------------------------------------------------------
struct WidgetSnapshot {
    float cpu_percent = 0.0f;
    float memory_percent = 0.0f;
    float disk_percent = 0.0f;
    double network_bytes_per_sec = 0.0;
    bool temperature_valid = false;
    float temperature_c = 0.0f;
    // T-302d weather: the dock snapshot chain's own flags, verbatim. An invalid
    // or failed snapshot renders the "--°" placeholder, never a blank slot -
    // the same contract dock_view.cpp WeatherTextFor pins for the dock tile.
    bool weather_valid = false;
    bool weather_failed = false;
    float weather_temp_c = 0.0f;
    // T-302d media: read-only GSMTC state. `media_valid` means one poll has
    // answered at least once (false = no sidecar answer yet: no box at all);
    // a session that exists but exposes no title still shows the play glyph.
    bool media_valid = false;
    bool media_has_session = false;
    bool media_playing = false;
    std::wstring media_title;
    // T-302d calendar_brief: the local month/day at the last Tick (1-based).
    int calendar_month = 0;
    int calendar_day = 0;
};

// Pure helpers behind the sampling (unit tested; the API readers are thin).
float ComputeDiskPercent(std::uint64_t used_bytes, std::uint64_t total_bytes);
double ComputeNetworkRate(std::uint64_t prev_octets, std::uint64_t cur_octets,
                          double elapsed_seconds);

// Display truncation for a media title. A song title is user data of
// unbounded length; a centre row that let one through would push every widget
// after it out of the span (LayoutWidgetGroup drops, never squeezes). The
// result is at most kMediaTitleMaxChars code units of the original plus one
// ellipsis when truncated - a pure string rule, unit tested.
constexpr std::size_t kMediaTitleMaxChars = 24;
std::wstring TruncateMediaTitle(const std::wstring& title);

// One widget's single-line label. Empty means "nothing to show" - the caller
// (BuildWidgetItems) drops the widget rather than drawing an empty box, which is
// what makes an unavailable temperature sensor render as nothing at all.
//
// The T-302d types deliberate deviation: weather NEVER returns empty (the spec
// pins a no-data placeholder, same as the dock tile), calendar_brief never
// (month/day always exist), and media returns empty only when there is no
// answer at all or no session - a session without a title still shows its play
// glyph rather than vanishing.
std::wstring WidgetDisplayText(WidgetType type, const WidgetSnapshot& snapshot);

// ---------------------------------------------------------------------------
// Layout: the centre zone is a third row of DIP boxes, anchored to the CENTRE of
// the free span between the left group and the right region, growing outwards.
// `order` counts FROM THE LEFT (ascending = left to right), the natural reading
// for a centre row; ties resolve by C-31's declaration sequence, deterministically
// (same rule as the right region). An entry that would cross the right limit is
// dropped, never squeezed; when the whole row is wider than the span the anchor
// clamps to the left limit and the row degrades rightwards with drops reported.
// ---------------------------------------------------------------------------

struct WidgetItem {
    WidgetType type = WidgetType::Cpu;
    int order = 0;
    std::wstring text;     // already rendered; empty entries are skipped
    UsageGrade grade = UsageGrade::Normal;
    // T-302d: weather draws dock::DrawWeatherGlyph left of its text (the same
    // glyph + value order the right region's weather_brief uses). Only Weather
    // sets this today; the flag is generic so a future icon widget reuses it.
    bool has_icon = false;
};

struct WidgetBox {
    render::RectF rect{};
    WidgetType type = WidgetType::Cpu;
    std::wstring text;
    UsageGrade grade = UsageGrade::Normal;
    bool has_icon = false;
    // Inner glyph square, valid when has_icon: 16 DIP (kRightIconBoxDip),
    // vertically centred in the bar - the painter draws into this exact rect.
    render::RectF icon{};
};

struct WidgetGroupSummary {
    float total_width_dip = 0.0f;
    float start_dip = 0.0f;  // where the row begins, in DIP from the bar's left edge
    std::size_t placed = 0;
    std::size_t dropped = 0;
};

// Renders the configured finderbar.widgets table into layout inputs against one
// snapshot. Unregistered types and entries whose text came out empty (an
// unavailable sensor, a media poll with no session) produce no item at all.
// The weather entry is the deliberate exception: its placeholder text is never
// empty, so a configured weather widget renders "--°" + the Unknown glyph
// rather than disappearing when the fetch chain has no data (the ask's
// 无数据占位 rule).
std::vector<WidgetItem> BuildWidgetItems(const core::FinderBarConfig& config,
                                         const WidgetSnapshot& snapshot);

// Lays the centre row out between the two limits and returns the boxes in
// left-to-right order. `measure` is the same seam LayoutRightGroup takes:
// production passes real text metrics, tests a deterministic fake.
std::vector<WidgetBox> LayoutWidgetGroup(const std::vector<WidgetItem>& items,
                                         float height_dip, float left_limit_dip,
                                         float right_limit_dip, MeasureFn measure,
                                         WidgetGroupSummary* summary);

// True when the table contains at least one type this build renders. WidgetHost
// gates its whole lifecycle on this (no registered widget = no thread, no timer),
// and FinderBar::ReloadConfig re-arms the host through it after a config edit.
bool AnyRegisteredWidget(const std::vector<core::FinderBarWidgetItem>& widgets);

// Side padding inside one widget box (same rhythm as the right region's
// kRightContentPaddingDip so the three zones share one breath).
constexpr float kWidgetContentPaddingDip = 6.0f;
// Gap between adjacent widget boxes.
constexpr float kWidgetGapDip = 12.0f;
// Widgets draw their label at the same size as every other text on the bar.
constexpr float kWidgetFontDip = kMenuFontDip;

// The read-only GSMTC answer the media worker publishes (T-302d). A namespace-
// level type because the worker thread's entry point is a free function; the
// fields are exactly what WidgetDisplayText(Media) consumes - poll_valid means
// one poll has answered (false = no sidecar answer yet: no widget box).
struct MediaWidgetState {
    bool poll_valid = false;
    bool has_session = false;
    bool playing = false;
    std::wstring title;
};

// ---------------------------------------------------------------------------
// WidgetHost - the sampling engine. One per bar; the bar owns it.
//
//   * Tick() runs on the bar's main thread (the Tick chain, never Paint) and
//     samples the four light quantities at the refresh period.
//   * A dedicated worker thread owns the WMI temperature query and publishes one
//     int32 (milli-degrees Celsius; kTemperatureInvalid when unusable) through an
//     atomic - the lock-free snapshot exchange the ask prescribes. It is started
//     only when the temperature widget is actually configured: a weather-only
//     table must not pay for a WMI thread.
//   * A second worker thread owns the T-302d media read path: it runs a private
//     MediaSidecarBridge (hw_sidecar.exe, the T-068 GSMTC transport - MinGW
//     cannot complete WinRT async, spikes/T-068), polls READ-ONLY at the refresh
//     period and publishes {has_session, playing, title} under a mutex. It never
//     sends "media <action>" - this package displays, T-304b controls (the DoD's
//     "media 简显只读不控制；写路径失败不报错到栏面": there is no write path
//     here at all, and a dead sidecar renders as "no session", never as an
//     error on the bar). Started only when the media widget is configured;
//     a missing hw_sidecar.exe means the widget renders nothing.
//   * DOCKFORGE_WIDGETS_DEMO=<normal|warn|critical|media> replaces the sampled
//     values with fixed ones for evidence capture (the grade tiers and a media
//     session cannot be produced deterministically from live hardware). It is
//     logged as a WARN when active and never ships in a default run.
// ---------------------------------------------------------------------------
class WidgetHost {
  public:
    WidgetHost() = default;
    ~WidgetHost();
    WidgetHost(const WidgetHost&) = delete;
    WidgetHost& operator=(const WidgetHost&) = delete;

    // Starts the workers the configured table needs. Idempotent. With no
    // registered widget type in `config` it starts NOTHING - no thread, no
    // timer, no sampling - which is what makes the evidence A/B (widgets on vs
    // off) a clean zero-cost baseline.
    void Start(const core::FinderBarConfig& config);
    void Stop();

    // Advances the sampler. Called from FinderBar::Tick on the main thread.
    // Returns true when a widget's rendered text or grade changed - the caller
    // re-lays the centre row and repaints. Text-level comparison, not value-level:
    // a CPU fluctuation inside one rounded percent must not churn the layout.
    bool Tick(double delta_ms, const core::FinderBarConfig& config);

    // Pushes the weather scalars the bar received from main.cpp (the T-075
    // snapshot chain; the bar forwards from FinderBar::SetWeatherSnapshot). Copy
    // only, main thread - no pointers cross a thread boundary (C-04b).
    void SetWeatherSnapshot(const dock::weather::Snapshot& snapshot);

    const WidgetSnapshot& Snapshot() const { return snapshot_; }

    // One-line JSON for the probe state file.
    std::string SnapshotJson() const;

  private:
    void SampleLight();
    void ApplyDemoIfAny();

    WidgetSnapshot snapshot_{};        // main thread only (Tick chain)
    WidgetSnapshot published_{};       // last state the rendered text was built from
    bool has_published_ = false;

    // CPU counters (GetSystemTimes diff via system_stats' own reader functions).
    SystemCounters prev_counters_{};
    bool has_prev_counters_ = false;
    // Network octets (GetIfTable2 sum) and the tick when they were read.
    std::uint64_t prev_octets_ = 0;
    std::uint64_t prev_net_tick_ms_ = 0;
    bool has_prev_net_ = false;

    double accumulator_ms_ = 0.0;
    int refresh_ms_ = 2000;
    bool running_ = false;
    bool any_registered_ = false;
    bool thermal_registered_ = false;
    bool media_registered_ = false;

    // Thermal worker (the one blocking source; owns its own COM apartment).
    std::thread thermal_thread_;
    std::atomic<bool> thermal_stop_{false};
    // Milli-degrees Celsius, or kTemperatureInvalid. One int32 = one atomic = no
    // torn read and no lock; the writer stores, the Tick chain loads.
    std::atomic<std::int32_t> thermal_milli_c_{kTemperatureInvalid};
    std::atomic<int> thermal_refresh_ms_{2000};

    // Media worker (T-302d): owns its own sidecar process on its own thread; the
    // poll answer crosses to the Tick chain under this mutex. The state it
    // publishes is read-only by construction - the thread has no API to send
    // media commands, which is how the DoD's "只读不控制" is enforced.
    std::thread media_thread_;
    std::atomic<bool> media_stop_{false};
    std::mutex media_mutex_;
    MediaWidgetState media_state_{};
    std::atomic<int> media_refresh_ms_{2000};

  public:
    // Sentinel the thermal thread publishes when WMI has no usable reading.
    static constexpr std::int32_t kTemperatureInvalid = INT32_MIN;
};

}  // namespace dockforge::finderbar
