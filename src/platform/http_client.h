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

}  // namespace dockforge::platform
