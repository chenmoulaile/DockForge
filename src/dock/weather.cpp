// DockForge - weather model, parser and refresh host (T-075).
#include "dock/weather.h"

#include <nlohmann/json.hpp>

#include "core/log.h"
#include "platform/http_client.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace dockforge::dock::weather {

namespace {

constexpr wchar_t kGeoHost[] = L"geocoding-api.open-meteo.com";
constexpr wchar_t kForecastHost[] = L"api.open-meteo.com";
constexpr int kRequestTimeoutMs = 15000;
constexpr int kMaxForecastDays = 3;

std::string NowLocalIso() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buffer[40] = {};
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth,
                  st.wDay, st.wHour, st.wMinute);
    return buffer;
}

}  // namespace

const char* ToString(Kind kind) {
    switch (kind) {
        case Kind::Clear: return "clear";
        case Kind::PartlyCloudy: return "partly_cloudy";
        case Kind::Cloudy: return "cloudy";
        case Kind::Fog: return "fog";
        case Kind::Rain: return "rain";
        case Kind::Snow: return "snow";
        case Kind::Thunder: return "thunder";
        case Kind::Unknown: return "unknown";
    }
    return "unknown";
}

Kind KindFromWmo(int code) {
    if (code == 0) return Kind::Clear;
    if (code == 1 || code == 2) return Kind::PartlyCloudy;
    if (code == 3) return Kind::Cloudy;
    if (code == 45 || code == 48) return Kind::Fog;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code == 56 || code == 57) {
        return Kind::Rain;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return Kind::Snow;
    if (code >= 95) return Kind::Thunder;
    return Kind::Unknown;
}

std::string UrlEncode(const std::string& utf8) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

std::wstring GeocodePath(const std::string& city_utf8) {
    std::wstring path = L"/v1/search?name=";
    // Percent-escapes are ASCII, so the narrow->wide widening below is exact.
    const std::string encoded = UrlEncode(city_utf8);
    path.append(encoded.begin(), encoded.end());
    path += L"&count=1&language=zh&format=json";
    return path;
}

std::wstring ForecastPath(double lat, double lon, int days) {
    wchar_t buffer[256] = {};
    const int clamped_days = days > 0 && days <= kMaxForecastDays ? days : kMaxForecastDays;
    std::swprintf(buffer, std::size(buffer),
                  L"/v1/forecast?latitude=%.4f&longitude=%.4f"
                  L"&current=temperature_2m,weather_code"
                  L"&daily=weather_code,temperature_2m_max,temperature_2m_min"
                  L"&timezone=auto&forecast_days=%d",
                  lat, lon, clamped_days);
    return buffer;
}

namespace {

// Days since 1970-01-01 for a "YYYY-MM-DD" string, or -1 when unparseable.
// Civil-days arithmetic (no time zones, no DST): the label only needs the
// difference between two local dates.
long DaysFromCivil(const std::string& date) {
    int year = 0, month = 0, day = 0;
    if (std::sscanf(date.c_str(), "%d-%d-%d", &year, &month, &day) != 3) return -1;
    if (month < 1 || month > 12 || day < 1 || day > 31) return -1;
    // Sakamoto's algorithm, shifted to a day number.
    const long y = static_cast<long>(year) - (month <= 2 ? 1 : 0);
    const long m = static_cast<long>(month);
    const long d = static_cast<long>(day);
    const long era = (y >= 0 ? y : y - 399) / 400;
    const long yoe = y - era * 400;
    const long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

}  // namespace

std::wstring WeekdayLabel(const std::string& date, const std::string& today) {
    const long target = DaysFromCivil(date);
    if (target < 0) {
        return std::wstring(date.begin(), date.end());
    }
    const long now = DaysFromCivil(today);
    const long delta = now >= 0 ? target - now : -1;
    if (delta == 0) return L"今天";
    if (delta == 1) return L"明天";
    if (delta >= 0 && delta <= 6) {
        // 1970-01-01 was a Thursday (weekday 4 with Sunday = 0).
        static const wchar_t* kNames[7] = {L"周日", L"周一", L"周二", L"周三",
                                           L"周四", L"周五", L"周六"};
        const long weekday = ((target % 7) + 7 + 4) % 7;
        return kNames[weekday];
    }
    return std::wstring(date.begin(), date.end());
}

bool ParseGeocode(const std::string& json, double& lat, double& lon, std::string& city) {
    try {
        const nlohmann::json document = nlohmann::json::parse(json);
        if (!document.is_object() || !document.contains("results")) return false;
        const auto& results = document.at("results");
        if (!results.is_array() || results.empty()) return false;
        const auto& first = results.front();
        if (!first.is_object() || !first.contains("latitude") || !first.contains("longitude")) {
            return false;
        }
        const double parsed_lat = first.at("latitude").get<double>();
        const double parsed_lon = first.at("longitude").get<double>();
        if (!std::isfinite(parsed_lat) || !std::isfinite(parsed_lon)) return false;
        std::string parsed_city;
        if (first.contains("name") && first.at("name").is_string()) {
            parsed_city = first.at("name").get<std::string>();
        }
        lat = parsed_lat;
        lon = parsed_lon;
        city = std::move(parsed_city);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool ParseForecast(const std::string& json, Snapshot& out) {
    try {
        const nlohmann::json document = nlohmann::json::parse(json);
        if (!document.is_object()) return false;
        if (!document.contains("current") || !document.contains("daily")) return false;
        const auto& current = document.at("current");
        if (!current.is_object() || !current.contains("temperature_2m") ||
            !current.contains("weather_code")) {
            return false;
        }
        const double temp = current.at("temperature_2m").get<double>();
        const int code = current.at("weather_code").get<int>();
        if (!std::isfinite(temp)) return false;

        const auto& daily = document.at("daily");
        if (!daily.is_object() || !daily.contains("time") || !daily.contains("weather_code") ||
            !daily.contains("temperature_2m_max") || !daily.contains("temperature_2m_min")) {
            return false;
        }
        const auto& time = daily.at("time");
        const auto& codes = daily.at("weather_code");
        const auto& tmax = daily.at("temperature_2m_max");
        const auto& tmin = daily.at("temperature_2m_min");
        if (!time.is_array() || !codes.is_array() || !tmax.is_array() || !tmin.is_array()) {
            return false;
        }
        const std::size_t count =
            std::min({time.size(), codes.size(), tmax.size(), tmin.size(),
                      static_cast<std::size_t>(kMaxForecastDays)});

        Snapshot parsed;
        parsed.temp_c = temp;
        parsed.kind = KindFromWmo(code);
        parsed.days.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            DayForecast day;
            if (time[i].is_string()) day.date = time[i].get<std::string>();
            day.kind = KindFromWmo(codes[i].is_number() ? codes[i].get<int>() : -1);
            day.tmax = tmax[i].is_number() ? tmax[i].get<double>() : 0.0;
            day.tmin = tmin[i].is_number() ? tmin[i].get<double>() : 0.0;
            parsed.days.push_back(std::move(day));
        }
        parsed.valid = true;
        parsed.failed = false;
        parsed.error.clear();
        out = std::move(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

Host::Fetcher DefaultFetcher() {
    // One-time host override read: the failure-path probe sets it before the
    // process starts, and re-reading per fetch would only add races.
    std::wstring override_host;
    wchar_t buffer[256] = {};
    const DWORD n = GetEnvironmentVariableW(L"DOCKFORGE_WEATHER_API_HOST", buffer,
                                            static_cast<DWORD>(std::size(buffer)));
    if (n > 0 && n < std::size(buffer)) {
        override_host.assign(buffer, n);
        DF_WARN("weather", "API host overridden by DOCKFORGE_WEATHER_API_HOST (probe hook)");
    }
    return [override_host](const std::wstring& host, const std::wstring& path, std::string& body,
                           std::string& error) -> bool {
        const std::wstring& target = override_host.empty() ? host : override_host;
        const platform::HttpTextResult result =
            platform::HttpsGetText(target.c_str(), path.c_str(), kRequestTimeoutMs);
        if (!result.ok) {
            error = "http status=" + std::to_string(result.status) +
                    " err=" + std::to_string(result.error) +
                    " ms=" + std::to_string(result.elapsed_ms);
            return false;
        }
        body = std::move(result.body);
        return true;
    };
}

Host::~Host() { Stop(); }

void Host::SetFetcher(Fetcher fetcher) { fetch_ = std::move(fetcher); }

void Host::Start(const core::WeatherConfig& config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return;
        config_ = config;
        // Arm the interval BEFORE the worker exists: the first Tick must find a
        // full countdown, because Start() already asked for one fetch now. A
        // zero here would make the first Tick publish a second request that the
        // interval never asked for.
        const double minutes = config.refresh_minutes > 0 ? config.refresh_minutes : 30;
        countdown_ms_ = minutes * 60.0 * 1000.0;
        refresh_requested_ = true;  // first fetch on start, not after one interval
        stop_ = false;
        running_ = true;
    }
    worker_ = std::thread([this] { RunWorker(); });
}

void Host::Reconfigure(const core::WeatherConfig& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config_.refresh_minutes != config.refresh_minutes) {
        // New interval: re-arm the countdown so the changed number takes effect
        // from now. Without this, shortening 360 -> 5 would still wait out the
        // old countdown, and lengthening 5 -> 360 would fire at the old due.
        // Runs on the UI thread before the worker sees the new config, so no
        // fetch can slip between the two.
        const double minutes = config.refresh_minutes > 0 ? config.refresh_minutes : 30;
        countdown_ms_ = minutes * 60.0 * 1000.0;
    }
    if (config_.city != config.city) {
        coords_valid_ = false;  // new city: re-run the geocoder on the next fetch
        resolved_city_.clear();
    }
    config_ = config;
}

void Host::RequestRefresh() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return;
        refresh_requested_ = true;
    }
    wake_.notify_one();
}

void Host::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ && !stop_) return;
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
    running_ = false;
}

bool Host::Tick(double delta_ms, Snapshot& out) {
    // Interval countdown lives on the UI thread: the worker knows nothing
    // about time, it only reacts to wake-ups, so a changed interval takes
    // effect without any cross-thread clock sharing. Start() and Reconfigure()
    // keep it positive whenever the host runs, so a non-positive value means
    // "no worker to schedule against" and the countdown simply holds.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            countdown_ms_ -= static_cast<float>(delta_ms);
            if (countdown_ms_ <= 0.0) {
                const double minutes =
                    config_.refresh_minutes > 0 ? config_.refresh_minutes : 30;
                countdown_ms_ = minutes * 60.0 * 1000.0;
                refresh_requested_ = true;
                wake_.notify_one();
            }
        }
        if (seen_generation_ == generation_) return false;
        seen_generation_ = generation_;
        out = latest_;
    }
    return true;
}

void Host::RunWorker() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return stop_ || refresh_requested_; });
        if (stop_) return;
        refresh_requested_ = false;

        const core::WeatherConfig config = config_;
        const bool coords_valid = coords_valid_;
        const double lat = lat_, lon = lon_;
        const std::string resolved_city = resolved_city_;
        lock.unlock();

        Snapshot snap;
        snap.updated_at = NowLocalIso();
        snap.city = resolved_city;
        // Locals the publish step needs; declared here because the geocode
        // leg that fills them lives inside a branch below.
        double use_lat = lat, use_lon = lon;
        std::string city = resolved_city;
        bool have_coords = coords_valid;

        if (config.provider == "amap") {
            // Keyed provider (T-075 DoD 1): the entry point exists - key read
            // from secrets.json by the caller - but the API chain is not
            // implemented this session; honest placeholder, never a fake read.
            snap.failed = true;
            snap.error = "amap provider not implemented; using open_meteo is recommended";
        } else if (config.city.empty()) {
            snap.failed = true;
            snap.error = "no city configured";
        } else {
            if (!have_coords) {
                std::string body, error;
                if (!fetch_ || !fetch_(kGeoHost, GeocodePath(config.city), body, error)) {
                    snap.failed = true;
                    snap.error = "geocode failed: " + (fetch_ ? error : std::string("no fetcher"));
                } else if (!ParseGeocode(body, use_lat, use_lon, city)) {
                    snap.failed = true;
                    snap.error = "geocode response unusable for '" + config.city + "'";
                } else {
                    have_coords = true;
                }
            }
            if (have_coords && !snap.failed) {
                std::string body, error;
                if (!fetch_ || !fetch_(kForecastHost, ForecastPath(use_lat, use_lon,
                                                                   kMaxForecastDays),
                                        body, error)) {
                    snap.failed = true;
                    snap.error = "forecast failed: " + (fetch_ ? error : std::string("no fetcher"));
                } else if (!ParseForecast(body, snap)) {
                    snap.failed = true;
                    snap.error = "forecast response unusable";
                } else {
                    snap.city = city.empty() ? config.city : city;
                }
            }
        }

        lock.lock();
        // Preserve validity across a failed refresh: the last good reading
        // stays on the tile (with the placeholder flag set) so a transient
        // network error does not erase known weather.
        if (snap.failed) {
            const bool had_valid = latest_.valid;
            const double old_temp = latest_.temp_c;
            const Kind old_kind = latest_.kind;
            const std::vector<DayForecast> old_days = latest_.days;
            const std::string old_city = latest_.city;
            latest_ = snap;
            if (had_valid) {
                latest_.valid = true;
                latest_.temp_c = old_temp;
                latest_.kind = old_kind;
                latest_.days = old_days;
                latest_.city = old_city;
            }
        } else {
            latest_ = snap;
        }
        // Cache the geocode result even when the forecast leg failed: the
        // coordinates are still good, and retrying the forecast must not pay
        // for another geocoder round trip.
        if (have_coords) {
            lat_ = use_lat;
            lon_ = use_lon;
            coords_valid_ = true;
            resolved_city_ = city;
        }
        ++generation_;
    }
}

}  // namespace dockforge::dock::weather
