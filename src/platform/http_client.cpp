// DockForge - minimal synchronous HTTPS GET over WinHTTP (T-075).
#include "platform/http_client.h"

#include <winhttp.h>

namespace dockforge::platform {

HttpTextResult HttpsGetText(const wchar_t* host, const wchar_t* path, int timeout_ms) {
    HttpTextResult out;
    const ULONGLONG t0 = GetTickCount64();
    const int timeout = timeout_ms > 0 ? timeout_ms : 15000;

    HINTERNET session = WinHttpOpen(L"DockForge/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.error = GetLastError();
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }
    // Resolve 5s, everything else caller-controlled: the spike measured DNS
    // failure (12007) returning in 16ms, so the resolve budget never dominates.
    WinHttpSetTimeouts(session, 5000, timeout, timeout, timeout);

    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        out.error = GetLastError();
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        out.error = GetLastError();
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                            0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        out.error = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
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
    out.ok = status >= 200 && status < 300;
    out.elapsed_ms = GetTickCount64() - t0;
    return out;
}

}  // namespace dockforge::platform
