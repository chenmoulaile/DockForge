// DockForge - weather model, parser and refresh host (T-075).
//
// Layering: the HTTP transport is platform::HttpsGetText (src/platform, C-06),
// the config keys are core::WeatherConfig (C-01), and this module owns
// everything weather-shaped: the Open-Meteo URL builders and JSON parsers
// (pure functions, unit-tested against captured response shapes), and the
// Host that runs the fetches on a worker thread and publishes immutable
// snapshots to the UI thread.
//
// Threading contract (same shape as the backdrop capture, C-04b):
//   * Start() spawns ONE worker thread. All network I/O happens there.
//   * The UI thread calls Tick() once per frame; it never blocks and never
//     allocates beyond the snapshot copy it hands out.
//   * Results cross the boundary as copies guarded by a mutex, never as
//     pointers, and never as callbacks into the UI thread.
#pragma once

#include "core/config.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dockforge::dock::weather {

// WMO 4677 weather-code families, collapsed to what one glyph can express.
// The dock shows a symbol, not a forecast paragraph; 0..99 codes that read
// identically at 48px (drizzle vs rain) share a Kind on purpose.
enum class Kind { Clear, PartlyCloudy, Cloudy, Fog, Rain, Snow, Thunder, Unknown };
const char* ToString(Kind kind);
Kind KindFromWmo(int code);

// One day of the hover-preview forecast.
struct DayForecast {
    std::string date;  // "YYYY-MM-DD" as Open-Meteo reports it
    Kind kind = Kind::Unknown;
    double tmax = 0.0;  // degrees Celsius
    double tmin = 0.0;
};

// Everything the dock and the hover preview render. Copied across the thread
// boundary as a whole; `failed` snapshots are still published because the
// failure placeholder is a render state, not an absence of state.
struct Snapshot {
    bool valid = false;  // a successful fetch has been adopted at least once
    bool failed = false;  // the most recent attempt failed (placeholder icon)
    double temp_c = 0.0;
    Kind kind = Kind::Unknown;
    std::string city;        // resolved display name (from geocoding)
    std::string updated_at;  // ISO-ish local time of the fetch
    std::string error;       // failure detail; empty on success
    std::vector<DayForecast> days;
};

// Percent-encodes a UTF-8 string for a query component (city names are not
// ASCII; "北京" must reach the geocoder as %E5%8C%97%E4%BA%AC).
std::string UrlEncode(const std::string& utf8);

std::wstring GeocodePath(const std::string& city_utf8);
std::wstring ForecastPath(double lat, double lon, int days);

// "今天" / "明天" / "周三" for a "YYYY-MM-DD" date, given today's date in the
// same format. Returns the date itself (YYYY-MM-DD) when it is not parseable
// or further than 6 days out - honest rather than wrong.
std::wstring WeekdayLabel(const std::string& date, const std::string& today);

// Parsers. Both return false and leave outputs untouched on malformed or
// unusable input - a bad response must degrade to the placeholder, never to
// a half-populated tile.
bool ParseGeocode(const std::string& json, double& lat, double& lon, std::string& city);
bool ParseForecast(const std::string& json, Snapshot& out);

// The production fetcher (see after the class it returns a member type of):
// platform::HttpsGetText with a 15s timeout. The DOCKFORGE_WEATHER_API_HOST
// environment variable overrides the API host for both endpoints - the
// failure-path probe points it at a dead name and exercises the placeholder
// without touching the network's real DNS.

class Host {
  public:
    // (host, path) -> body. Returns false with `error` filled on any failure.
    using Fetcher = std::function<bool(const std::wstring& host, const std::wstring& path,
                                       std::string& body, std::string& error)>;

    Host() = default;
    ~Host();  // Stop()s and joins the worker; an unused host is free.

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Must be called before Start. An empty fetcher makes every fetch fail
    // with "no fetcher" (tests inject fakes here).
    void SetFetcher(Fetcher fetcher);

    // Spawns the worker and requests the first fetch immediately: a tile that
    // appears without data has a placeholder, and the placeholder should be
    // replaced within one round trip, not one interval.
    void Start(const core::WeatherConfig& config);

    // Adopts new settings (city, interval, provider). A changed city clears
    // the cached coordinates; an unchanged city keeps them, so an interval
    // edit does not re-run the geocoder.
    void Reconfigure(const core::WeatherConfig& config);

    // Wakes the worker for one fetch now, regardless of the interval (the
    // manual-refresh path: a click on the tile). Coalesced while a fetch is
    // already in flight.
    void RequestRefresh();

    void Stop();

    // UI thread, once per frame. Decrements the refresh countdown and adopts
    // a newly published snapshot. Returns true exactly when `out` changed.
    bool Tick(double delta_ms, Snapshot& out);

  private:
    void RunWorker();

    Fetcher fetch_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    bool running_ = false;
    bool stop_ = false;
    bool refresh_requested_ = false;
    core::WeatherConfig config_{};
    // Geocode cache keyed by the city it resolved.
    std::string resolved_city_;
    double lat_ = 0.0, lon_ = 0.0;
    bool coords_valid_ = false;
    Snapshot latest_;
    unsigned long long generation_ = 0;   // bumped on every publish
    unsigned long long seen_generation_ = 0;  // UI-thread copy
    // UI thread only. Start() arms it to the full interval; it answers "when is
    // the NEXT automatic refresh due", because Start() already asked the worker
    // for one fetch immediately.
    double countdown_ms_ = 0.0;
};

// The production fetcher: platform::HttpsGetText with a 15s timeout. The
// DOCKFORGE_WEATHER_API_HOST environment variable overrides the API host for
// both endpoints - the failure-path probe points it at a dead name and
// exercises the placeholder without touching the network's real DNS.
Host::Fetcher DefaultFetcher();

}  // namespace dockforge::dock::weather
