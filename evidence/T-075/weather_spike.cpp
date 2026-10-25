// DockForge T-075 spike: WinHTTP (MinGW-w64) against the Open-Meteo public
// APIs - geocoding by city name, then the forecast endpoint over HTTPS.
//
// DoD questions answered by running this binary:
//   Q1. Does MinGW-w64 compile <winhttp.h> and can a console process complete
//       an HTTPS GET against api.open-meteo.com with sane timeouts?
//   Q2. Does the geocoding API resolve a manually-specified city name
//       (Open-Meteo needs lat/lon; the DoD's "city manually specified" for
//       the no-key fallback has to work end to end)?
//   Q3. Does the forecast endpoint return current temp + weather_code plus a
//       3-day daily min/max that we can render in the hover preview?
//   Q4. What does a dead host do - WinHTTP error code and wall time - so the
//       failure placeholder path can be designed around a real number?
//   Q5. What does one request cost in wall time (success case), to size the
//       worker-thread budget?
//
// Build (Git Bash, repo root):
//   g++ -std=c++17 -O2 -o spikes/T-075_weather_probe/weather_spike.exe \
//       spikes/T-075_weather_probe/weather_spike.cpp -lwinhttp
#include <windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace {

struct FetchResult {
    bool ok = false;
    DWORD status = 0;
    DWORD last_error = 0;
    std::string body;
    ULONGLONG elapsed_ms = 0;
};

// One synchronous HTTPS GET. Everything (session, connect, request) is opened
// and closed inside, so the function is its own failure boundary.
FetchResult HttpsGet(const wchar_t* host, int port, const wchar_t* path, DWORD timeout_ms) {
    FetchResult out;
    const ULONGLONG t0 = GetTickCount64();

    HINTERNET session = WinHttpOpen(L"DockForge-spike/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.last_error = GetLastError();
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }
    WinHttpSetTimeouts(session, 5000, static_cast<int>(timeout_ms),
                       static_cast<int>(timeout_ms), static_cast<int>(timeout_ms));

    HINTERNET connect = WinHttpConnect(session, host, static_cast<INTERNET_PORT>(port), 0);
    if (!connect) {
        out.last_error = GetLastError();
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        out.last_error = GetLastError();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        out.last_error = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = status;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) break;
        chunk.resize(read);
        out.body += chunk;
        if (read == 0) break;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    out.ok = (status >= 200 && status < 300);
    out.elapsed_ms = GetTickCount64() - t0;
    return out;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    printf("T-075 weather spike (pid=%lu)\n", GetCurrentProcessId());

    // Q2: geocoding - the DoD's "city manually specified" path.
    FetchResult geo = HttpsGet(L"geocoding-api.open-meteo.com", 443,
                               L"/v1/search?name=Beijing&count=1&language=zh&format=json",
                               15000);
    printf("Q2 geocode: ok=%d status=%lu err=%lu ms=%llu len=%zu\n", geo.ok ? 1 : 0, geo.status,
           geo.last_error, geo.elapsed_ms, geo.body.size());
    const bool geo_has_latlon = Contains(geo.body, "latitude") && Contains(geo.body, "longitude");
    printf("   body head: %.160s\n", geo.body.substr(0, 160).c_str());
    printf("   has latitude+longitude fields: %d\n", geo_has_latlon ? 1 : 0);

    // Q1+Q3: forecast with the coordinates Beijing resolved to (fixed here; the
    // product will take them from the geocode response).
    FetchResult fc = HttpsGet(
        L"api.open-meteo.com", 443,
        L"/v1/forecast?latitude=39.9042&longitude=116.4074&current=temperature_2m,weather_code"
        L"&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto"
        L"&forecast_days=3",
        15000);
    printf("Q1+Q3 forecast: ok=%d status=%lu err=%lu ms=%llu len=%zu\n", fc.ok ? 1 : 0, fc.status,
           fc.last_error, fc.elapsed_ms, fc.body.size());
    const bool fc_current = Contains(fc.body, "temperature_2m") && Contains(fc.body, "weather_code");
    const bool fc_daily =
        Contains(fc.body, "temperature_2m_max") && Contains(fc.body, "temperature_2m_min");
    printf("   body head: %.200s\n", fc.body.substr(0, 200).c_str());
    printf("   has current fields: %d  has daily min/max: %d\n", fc_current ? 1 : 0,
           fc_daily ? 1 : 0);

    // Q4: dead host behaviour (failure placeholder path design input).
    FetchResult dead =
        HttpsGet(L"no-such-host.dockforge.invalid", 443, L"/v1/forecast", 8000);
    printf("Q4 dead host: ok=%d status=%lu err=%lu ms=%llu\n", dead.ok ? 1 : 0, dead.status,
           dead.last_error, dead.elapsed_ms);

    // Q1 verdict is "compiled and completed a real HTTPS round trip".
    const bool q1 = fc.ok && fc.status == 200;
    const bool q2 = geo.ok && geo_has_latlon;
    const bool q3 = fc_current && fc_daily;
    const bool q4 = !dead.ok && dead.last_error != 0;
    printf("verdict: Q1=%s Q2=%s Q3=%s Q4=%s\n", q1 ? "PASS" : "FAIL", q2 ? "PASS" : "FAIL",
           q3 ? "PASS" : "FAIL", q4 ? "PASS" : "FAIL");
    return (q1 && q2 && q3 && q4) ? 0 : 1;
}
