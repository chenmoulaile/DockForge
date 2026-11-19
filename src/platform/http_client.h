// DockForge - minimal synchronous HTTPS GET over WinHTTP (T-075).
//
// Contract: CONTRACTS.md C-06 puts Win32 wrappers in src/platform. This one
// carries no state and no business logic - callers hand it a host and a path
// and get status + body back. It is BLOCKING (resolves, connects, reads to
// completion), so it must only be called from a worker thread; the weather
// host owns that thread. The T-075 spike (spikes/T-075_weather_probe) measured
// the real endpoints through this exact call shape: ~1.2s for a forecast
// round trip, 16ms + ERROR_WINHTTP_NAME_NOT_RESOLVED for a dead host.
#pragma once

#include "platform/win_common.h"

#include <string>

namespace dockforge::platform {

struct HttpTextResult {
    bool ok = false;             // transport succeeded AND status is 2xx
    unsigned long status = 0;    // HTTP status code, 0 when the request never completed
    unsigned long error = 0;     // GetLastError()/WinHttp error on failure, 0 otherwise
    unsigned long long elapsed_ms = 0;
    std::string body;            // response body (possibly partial on failure)
};

// One HTTPS GET. Every handle is opened and closed inside the call, so a
// failure leaves nothing behind. `timeout_ms` applies to connect/send/receive
// individually (resolve is fixed at 5s); a dead host therefore surfaces in
// bounded time rather than hanging the worker.
HttpTextResult HttpsGetText(const wchar_t* host, const wchar_t* path, int timeout_ms);

// Byte-bodied GET over a full URL, http or https (T-085 theme distribution).
//
// Splits the URL itself - scheme, host, optional :port, path - so callers hand
// one string and never re-implement the splitting. Proxy mode is
// WINHTTP_ACCESS_TYPE_NO_PROXY: the T-085 spike measured the DEFAULT_PROXY
// lookup intermittently timing out against raw.githubusercontent.com (12002,
// twice in a row while curl succeeded concurrently), and NO_PROXY answered
// 5/5. Every handle opens and closes inside the call; failures leave nothing
// behind and arrive in bounded time (same timeout shape as HttpsGetText).
struct HttpBytesResult {
    bool ok = false;             // transport succeeded AND status is 2xx
    unsigned long status = 0;    // HTTP status code, 0 when the request never completed
    unsigned long error = 0;     // GetLastError()/WinHttp error on failure, 0 otherwise
    unsigned long long elapsed_ms = 0;
    std::string body;            // response bytes (possibly partial on failure)
};

HttpBytesResult HttpGetBytes(const std::string& url_utf8, int timeout_ms);

}  // namespace dockforge::platform
