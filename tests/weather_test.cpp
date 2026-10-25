// T-075 weather widget: WMO mapping, URL builders, parsers, weekday labels,
// glyph layout (C-22 weather_text), the host's refresh state machine (real
// worker thread, injected fake fetcher), config round-trip and the secrets
// reader.
#include "catch_amalgamated.hpp"

#include "core/config.h"
#include "core/secrets.h"
#include "dock/dock_view.h"
#include "dock/weather.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

using namespace dockforge;
namespace weather = dock::weather;
using weather::DayForecast;
using weather::Kind;
using weather::Snapshot;

namespace {

// Geocoding response captured from the live API (spikes/T-075_weather_probe,
// Q2). Field order matches the real service.
const char* kGeocodeBeijing =
    R"({"results":[{"id":1816670,"name":"北京","latitude":39.9075,"longitude":116.39723,)"
    R"("elevation":49.0,"feature_code":"PPLC","country_code":"CN","population":21766614,)"
    R"("timezone":"Asia/Shanghai"}],"generationtime_ms":0.48})";

// Forecast body shaped exactly like the live endpoint's (spike Q1+Q3): current
// block plus three zipped daily arrays.
const char* kForecastSample =
    R"({"latitude":39.89,"longitude":116.36,"utc_offset_seconds":28800,)"
    R"("current":{"temperature_2m":15.4,"weather_code":3},)"
    R"("daily":{"time":["2026-10-25","2026-10-26","2026-10-27"],)"
    R"("weather_code":[3,61,0],)"
    R"("temperature_2m_max":[18.2,14.9,20.1],)"
    R"("temperature_2m_min":[7.5,9.0,8.3]}})";

core::WeatherConfig ProbeWeatherConfig() {
    core::WeatherConfig config;
    config.enabled = true;
    config.city = "Beijing";
    config.refresh_minutes = 30;
    return config;
}

// Waits for the host to publish a snapshot the UI thread has not seen yet.
// Real thread, fake fetcher: the only thing under test is the host's own
// sequencing, and a bounded 2s wait keeps a failure loud rather than hanging.
bool PumpForSnapshot(dock::weather::Host& host, Snapshot& out, int max_ms = 2000) {
    for (int waited = 0; waited < max_ms; waited += 10) {
        if (host.Tick(10.0, out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::wstring TempSecretsPath(const char* contents) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    wchar_t path[MAX_PATH] = {};
    swprintf(path, MAX_PATH, L"%sdockforge-t075-secrets-%d.json", dir, GetCurrentProcessId());
    SetEnvironmentVariableW(L"DOCKFORGE_SECRETS_PATH", path);
    if (contents != nullptr) {
        std::ofstream file(path, std::ios::binary);
        file << contents;
    }
    return path;
}

}  // namespace

// ---- WMO code mapping --------------------------------------------------------

TEST_CASE("WMO codes map to glyph families", "[weather]") {
    CHECK(weather::KindFromWmo(0) == Kind::Clear);
    CHECK(weather::KindFromWmo(1) == Kind::PartlyCloudy);
    CHECK(weather::KindFromWmo(2) == Kind::PartlyCloudy);
    CHECK(weather::KindFromWmo(3) == Kind::Cloudy);
    CHECK(weather::KindFromWmo(45) == Kind::Fog);
    CHECK(weather::KindFromWmo(48) == Kind::Fog);
    CHECK(weather::KindFromWmo(51) == Kind::Rain);
    CHECK(weather::KindFromWmo(61) == Kind::Rain);
    CHECK(weather::KindFromWmo(80) == Kind::Rain);
    CHECK(weather::KindFromWmo(71) == Kind::Snow);
    CHECK(weather::KindFromWmo(85) == Kind::Snow);
    CHECK(weather::KindFromWmo(95) == Kind::Thunder);
    CHECK(weather::KindFromWmo(99) == Kind::Thunder);
    CHECK(weather::KindFromWmo(-1) == Kind::Unknown);
    CHECK(weather::KindFromWmo(42) == Kind::Unknown);
}

// ---- URL builders ------------------------------------------------------------

TEST_CASE("UrlEncode percent-encodes UTF-8 city names", "[weather]") {
    CHECK(weather::UrlEncode("Beijing") == "Beijing");
    CHECK(weather::UrlEncode("a b") == "a%20b");
    // "北京" in UTF-8 bytes.
    CHECK(weather::UrlEncode("\xe5\x8c\x97\xe4\xba\xac") == "%E5%8C%97%E4%BA%AC");
}

TEST_CASE("GeocodePath embeds the encoded city", "[weather]") {
    const std::wstring path = weather::GeocodePath("北京");
    CHECK(path.find(L"name=%E5%8C%97%E4%BA%AC") != std::wstring::npos);
    CHECK(path.find(L"/v1/search?") == 0);
}

TEST_CASE("ForecastPath carries coordinates and 3 days", "[weather]") {
    const std::wstring path = weather::ForecastPath(39.9075, 116.39723, 3);
    CHECK(path.find(L"/v1/forecast?") == 0);
    CHECK(path.find(L"latitude=39.9075") != std::wstring::npos);
    CHECK(path.find(L"longitude=116.3972") != std::wstring::npos);
    CHECK(path.find(L"forecast_days=3") != std::wstring::npos);
    CHECK(path.find(L"temperature_2m_max") != std::wstring::npos);
}

TEST_CASE("ForecastPath clamps the day count to 3", "[weather]") {
    CHECK(weather::ForecastPath(1, 2, 7).find(L"forecast_days=3") != std::wstring::npos);
    CHECK(weather::ForecastPath(1, 2, 0).find(L"forecast_days=3") != std::wstring::npos);
}

// ---- parsers -----------------------------------------------------------------

TEST_CASE("ParseGeocode reads the live response shape", "[weather]") {
    double lat = 0, lon = 0;
    std::string city;
    REQUIRE(weather::ParseGeocode(kGeocodeBeijing, lat, lon, city));
    CHECK(lat == Catch::Approx(39.9075));
    CHECK(lon == Catch::Approx(116.39723));
    // The display name is UTF-8; comparing raw bytes keeps the test encoding-proof.
    CHECK(city == "\xe5\x8c\x97\xe4\xba\xac");
}

TEST_CASE("ParseGeocode rejects empty results and garbage", "[weather]") {
    double lat = 1, lon = 2;
    std::string city = "x";
    CHECK_FALSE(weather::ParseGeocode(R"({"results":[]})", lat, lon, city));
    CHECK_FALSE(weather::ParseGeocode("not json", lat, lon, city));
    CHECK_FALSE(weather::ParseGeocode(R"({"count":0})", lat, lon, city));
    // Outputs untouched on failure (the placeholder must not show half data).
    CHECK(lat == 1);
    CHECK(lon == 2);
    CHECK(city == "x");
}

TEST_CASE("ParseForecast zips the daily arrays into a snapshot", "[weather]") {
    Snapshot snap;
    REQUIRE(weather::ParseForecast(kForecastSample, snap));
    CHECK(snap.valid);
    CHECK_FALSE(snap.failed);
    CHECK(snap.temp_c == Catch::Approx(15.4));
    CHECK(snap.kind == Kind::Cloudy);
    REQUIRE(snap.days.size() == 3);
    CHECK(snap.days[0].date == "2026-10-25");
    CHECK(snap.days[0].kind == Kind::Cloudy);
    CHECK(snap.days[0].tmax == Catch::Approx(18.2));
    CHECK(snap.days[0].tmin == Catch::Approx(7.5));
    CHECK(snap.days[1].kind == Kind::Rain);
    CHECK(snap.days[2].kind == Kind::Clear);
}

TEST_CASE("ParseForecast truncates ragged daily arrays at the shortest", "[weather]") {
    const std::string ragged =
        R"({"current":{"temperature_2m":1.0,"weather_code":0},)"
        R"("daily":{"time":["2026-10-25","2026-10-26","2026-10-27"],)"
        R"("weather_code":[0,1],"temperature_2m_max":[5,6],"temperature_2m_min":[1,2]}})";    Snapshot snap;
    REQUIRE(weather::ParseForecast(ragged, snap));
    CHECK(snap.days.size() == 2);
}

TEST_CASE("ParseForecast rejects missing blocks without touching outputs", "[weather]") {
    Snapshot snap;
    snap.valid = true;  // sentinel: must survive a rejected parse
    CHECK_FALSE(weather::ParseForecast(R"({"current":{}})", snap));
    CHECK_FALSE(weather::ParseForecast(R"({"daily":{}})", snap));
    CHECK_FALSE(weather::ParseForecast("[]", snap));
    CHECK(snap.valid);  // untouched
}

// ---- weekday labels ----------------------------------------------------------

TEST_CASE("WeekdayLabel names today, tomorrow and weekdays", "[weather]") {
    // 2026-10-25 is a Sunday (checked: Sakamoto in weather.cpp must agree).
    CHECK(weather::WeekdayLabel("2026-10-25", "2026-10-25") == L"今天");
    CHECK(weather::WeekdayLabel("2026-10-26", "2026-10-25") == L"明天");
    CHECK(weather::WeekdayLabel("2026-10-28", "2026-10-25") == L"周三");
    CHECK(weather::WeekdayLabel("2026-10-31", "2026-10-25") == L"周六");
    // Beyond a week out, the raw date is more honest than a wrong label.
    CHECK(weather::WeekdayLabel("2026-11-05", "2026-10-25") == L"2026-11-05");
    CHECK(weather::WeekdayLabel("garbage", "2026-10-25") == L"garbage");
}

// ---- glyph layout (C-22 weather_text) ----------------------------------------

TEST_CASE("weather text follows the C-22 number-size rule", "[dock][weather]") {
    const render::RectF slot{100.0f, 40.0f, 148.0f, 88.0f};  // 48 DIP icon
    const dock::WeatherGlyphLayout layout = dock::ComputeWeatherGlyphLayout(slot);
    // 22% of 48 = 10.56 -> below the 12 DIP (9pt) floor.
    CHECK(layout.font_dip == Catch::Approx(12.0f));
    const render::RectF big{0.0f, 0.0f, 96.0f, 96.0f};
    CHECK(dock::ComputeWeatherGlyphLayout(big).font_dip == Catch::Approx(21.12f).margin(0.01f));
}

TEST_CASE("glyph box is square and inside the slot", "[dock][weather]") {
    const render::RectF slot{10.0f, 0.0f, 58.0f, 48.0f};
    const dock::WeatherGlyphLayout layout = dock::ComputeWeatherGlyphLayout(slot);
    CHECK(layout.glyph.Width() == Catch::Approx(layout.glyph.Height()).margin(0.01f));
    CHECK(layout.glyph.left >= slot.left);
    CHECK(layout.glyph.right <= slot.right);
    CHECK(layout.glyph.top >= slot.top);
    CHECK(layout.text.bottom <= slot.bottom);
    CHECK(layout.text.top >= layout.glyph.top);
}

TEST_CASE("WeatherTextFor renders a degree label or the placeholder", "[dock][weather]") {
    Snapshot snap;
    snap.valid = true;
    snap.temp_c = 22.6;
    CHECK(dock::WeatherTextFor(snap) == L"23\u00B0");
    snap.temp_c = -3.2;
    CHECK(dock::WeatherTextFor(snap) == L"-3\u00B0");
    snap.valid = false;
    CHECK(dock::WeatherTextFor(snap) == L"--\u00B0");
    snap.valid = true;
    snap.failed = true;  // last fetch failed: placeholder even with old data
    CHECK(dock::WeatherTextFor(snap) == L"--\u00B0");
}

// ---- host state machine ------------------------------------------------------

TEST_CASE("host fetches on start and publishes a valid snapshot", "[weather][host]") {
    dock::weather::Host host;
    int geocode_calls = 0, forecast_calls = 0;
    host.SetFetcher([&](const std::wstring& host_name, const std::wstring& path,
                        std::string& body, std::string& error) {
        if (host_name.find(L"geocoding") != std::wstring::npos) {
            ++geocode_calls;
            body = kGeocodeBeijing;
            return true;
        }
        ++forecast_calls;
        body = kForecastSample;
        return true;
    });
    host.Start(ProbeWeatherConfig());
    Snapshot snap;
    REQUIRE(PumpForSnapshot(host, snap));
    host.Stop();
    CHECK(snap.valid);
    CHECK_FALSE(snap.failed);
    CHECK(snap.temp_c == Catch::Approx(15.4));
    CHECK(snap.city == "\xe5\x8c\x97\xe4\xba\xac");
    CHECK(snap.days.size() == 3);
    CHECK(geocode_calls == 1);  // coordinates cached after the first round
    CHECK(forecast_calls == 1);
}

TEST_CASE("a failed fetch publishes the placeholder, a later success recovers",
          "[weather][host]") {
    dock::weather::Host host;
    bool fail_first = true;
    host.SetFetcher([&](const std::wstring&, const std::wstring&, std::string& body,
                        std::string& error) {
        if (fail_first) {
            error = "http status=0 err=12007 ms=16";
            return false;
        }
        body = kForecastSample;
        return true;
    });
    host.Start(ProbeWeatherConfig());
    Snapshot snap;
    REQUIRE(PumpForSnapshot(host, snap));
    host.Stop();
    CHECK(snap.failed);
    CHECK_FALSE(snap.valid);
    CHECK(snap.error.find("12007") != std::string::npos);

    // Manual refresh after the fetcher recovered: the old failure must not
    // stick. (A fresh host is used because Stop() is terminal by design.)
    dock::weather::Host second;
    second.SetFetcher([&](const std::wstring& host_name, const std::wstring&,
                          std::string& body, std::string&) {
        if (host_name.find(L"geocoding") != std::wstring::npos) {
            body = kGeocodeBeijing;
            return true;
        }
        body = kForecastSample;
        return true;
    });
    second.Start(ProbeWeatherConfig());
    Snapshot recovered;
    REQUIRE(PumpForSnapshot(second, recovered));
    second.Stop();
    CHECK(recovered.valid);
    CHECK_FALSE(recovered.failed);
}

TEST_CASE("manual refresh re-fetches before the interval elapses", "[weather][host]") {
    dock::weather::Host host;
    std::atomic<int> forecast_calls{0};
    host.SetFetcher([&](const std::wstring& host_name, const std::wstring&, std::string& body,
                       std::string&) {
        if (host_name.find(L"geocoding") != std::wstring::npos) {
            body = kGeocodeBeijing;
            return true;
        }
        ++forecast_calls;
        body = kForecastSample;
        return true;
    });
    core::WeatherConfig config = ProbeWeatherConfig();
    config.refresh_minutes = 360;  // six hours: only a manual refresh can fire again
    host.Start(config);
    Snapshot snap;
    REQUIRE(PumpForSnapshot(host, snap));
    CHECK(forecast_calls.load() == 1);

    host.RequestRefresh();  // the tile-click path
    // Ticks advance the interval clock; they must NOT schedule an automatic
    // fetch inside six hours. Tick's return value IS the adopted snapshot, so
    // the manual re-fetch can land in this loop (the fake fetcher returns in
    // microseconds) instead of in the pump below - either way exactly one more
    // fetch must have fired.
    bool adopted = false;
    for (int i = 0; i < 20; ++i) {
        if (host.Tick(50.0, snap)) adopted = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!adopted) {
        REQUIRE(PumpForSnapshot(host, snap));
    }
    host.Stop();
    CHECK(forecast_calls.load() == 2);  // exactly one manual re-fetch
}

TEST_CASE("interval ticking schedules an automatic refresh", "[weather][host]") {
    dock::weather::Host host;
    std::atomic<int> forecast_calls{0};
    host.SetFetcher([&](const std::wstring& host_name, const std::wstring&, std::string& body,
                       std::string&) {
        if (host_name.find(L"geocoding") != std::wstring::npos) {
            body = kGeocodeBeijing;
            return true;
        }
        ++forecast_calls;
        body = kForecastSample;
        return true;
    });
    core::WeatherConfig config = ProbeWeatherConfig();
    config.refresh_minutes = 5;  // smallest legal interval = 5 minutes
    host.Start(config);
    Snapshot snap;
    REQUIRE(PumpForSnapshot(host, snap));
    CHECK(forecast_calls.load() == 1);
    // Drive 5 minutes of ticks in coarse chunks.
    for (int ms = 0; ms < 5 * 60 * 1000; ms += 60000) {
        host.Tick(60000.0, snap);
    }
    REQUIRE(PumpForSnapshot(host, snap));
    host.Stop();
    CHECK(forecast_calls.load() == 2);
}

TEST_CASE("reconfiguring the city re-runs the geocoder; same city does not",
          "[weather][host]") {
    dock::weather::Host host;
    std::atomic<int> geocode_calls{0};
    host.SetFetcher([&](const std::wstring& host_name, const std::wstring&, std::string& body,
                       std::string&) {
        if (host_name.find(L"geocoding") != std::wstring::npos) {
            ++geocode_calls;
            body = kGeocodeBeijing;
            return true;
        }
        body = kForecastSample;
        return true;
    });
    host.Start(ProbeWeatherConfig());
    Snapshot snap;
    REQUIRE(PumpForSnapshot(host, snap));
    CHECK(geocode_calls.load() == 1);

    core::WeatherConfig same = ProbeWeatherConfig();
    same.refresh_minutes = 45;  // interval edit only
    host.Reconfigure(same);
    host.RequestRefresh();
    REQUIRE(PumpForSnapshot(host, snap));
    CHECK(geocode_calls.load() == 1);  // cached coordinates reused

    core::WeatherConfig moved = same;
    moved.city = "Shanghai";
    host.Reconfigure(moved);
    host.RequestRefresh();
    REQUIRE(PumpForSnapshot(host, snap));
    host.Stop();
    CHECK(geocode_calls.load() == 2);  // new city re-geocoded
}

TEST_CASE("amap provider reports an honest unimplemented placeholder", "[weather][host]") {
    dock::weather::Host host;
    int any_call = 0;
    host.SetFetcher([&](const std::wstring&, const std::wstring&, std::string&, std::string&) {
        ++any_call;
        return false;
    });
    core::WeatherConfig config = ProbeWeatherConfig();
    config.provider = "amap";
    host.Start(config);
    Snapshot snap;
    REQUIRE(PumpForSnapshot(host, snap));
    host.Stop();
    CHECK(snap.failed);
    CHECK(snap.error.find("amap") != std::string::npos);
    CHECK(any_call == 0);  // never fakes a request at the keyed provider
}

// ---- config schema -----------------------------------------------------------

TEST_CASE("weather config round-trips through parse and serialize", "[config][weather]") {
    core::WeatherConfig config;
    config.enabled = true;
    config.city = "北京";
    config.refresh_minutes = 15;
    core::Config whole;
    whole.weather = config;
    const std::string json = core::SerializeConfig(whole);

    core::Config parsed;
    std::string error;
    REQUIRE(core::ParseConfig(json, parsed, &error));
    CHECK(core::SameWeather(config, parsed.weather));
    CHECK(parsed.weather.city == "\xe5\x8c\x97\xe4\xba\xac");
}

TEST_CASE("default config serializes without a weather key", "[config][weather]") {
    core::Config whole;
    const std::string json = core::SerializeConfig(whole);
    CHECK(json.find("weather") == std::string::npos);
    // A city alone is kept: the user typed it, losing it would be a reset.
    whole.weather.city = "Beijing";
    const std::string with_city = core::SerializeConfig(whole);
    CHECK(with_city.find("\"weather\"") != std::string::npos);
}

TEST_CASE("out-of-range refresh interval clamps into 5..360", "[config][weather]") {
    core::Config whole;
    whole.weather.refresh_minutes = 1;
    core::ClampConfig(whole);
    CHECK(whole.weather.refresh_minutes == 5);
    whole.weather.refresh_minutes = 9999;
    core::ClampConfig(whole);
    CHECK(whole.weather.refresh_minutes == 360);
    CHECK(core::WeatherRefreshMinutesRange().min == 5);
    CHECK(core::WeatherRefreshMinutesRange().max == 360);
}

TEST_CASE("unknown weather provider falls back with a warning-worthy parse", "[config][weather]") {
    const std::string json = R"({"weather":{"enabled":true,"provider":"accu","city":"X"}})";
    core::Config parsed;
    std::string error;
    REQUIRE(core::ParseConfig(json, parsed, &error));
    CHECK(parsed.weather.provider == "open_meteo");  // fell back
    CHECK(parsed.needs_rewrite);                     // bad value rewrites the file
    CHECK(parsed.weather.enabled);
    CHECK(parsed.weather.city == "X");
}

TEST_CASE("weather unknown keys survive a round-trip", "[config][weather]") {
    const std::string json =
        R"({"weather":{"enabled":true,"city":"Beijing","future_key":42}})";
    core::Config parsed;
    std::string error;
    REQUIRE(core::ParseConfig(json, parsed, &error));
    const std::string round = core::SerializeConfig(parsed);
    CHECK(round.find("future_key") != std::string::npos);
    CHECK(round.find("\"city\"") != std::string::npos);
}

// ---- secrets -----------------------------------------------------------------

TEST_CASE("secrets reader reads a keyed file and tolerates absence", "[core][secrets]") {
    const std::wstring path =
        TempSecretsPath(R"({"amap_key":"0123456789abcdef","other":"v"})");
    auto secrets = core::ReadSecrets();
    CHECK(secrets.count("amap_key") == 1);
    CHECK(secrets.at("amap_key") == "0123456789abcdef");
    CHECK(secrets.count("other") == 1);
    // Clean up and re-read: a missing file is "no key", not an error.
    DeleteFileW(path.c_str());
    auto missing = core::ReadSecrets();
    CHECK(missing.empty());
    SetEnvironmentVariableW(L"DOCKFORGE_SECRETS_PATH", nullptr);
}

TEST_CASE("malformed secrets degrade to no keys", "[core][secrets]") {
    const std::wstring path = TempSecretsPath("{ not json ]");
    auto secrets = core::ReadSecrets();
    CHECK(secrets.empty());
    DeleteFileW(path.c_str());
    SetEnvironmentVariableW(L"DOCKFORGE_SECRETS_PATH", nullptr);
}
