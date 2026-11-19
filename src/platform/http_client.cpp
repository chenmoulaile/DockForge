// DockForge - minimal synchronous HTTPS GET over WinHTTP (T-075).
#include "platform/http_client.h"

#include <winhttp.h>

#include <cctype>

namespace dockforge::platform {
namespace {

// Local UTF-8 -> UTF-16 (keeps this TU free of a core/ include; C-06 keeps
// platform self-contained).
std::wstring WideFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(),
                        needed);
    return out;
}

}  // namespace

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

namespace {

struct ParsedUrl {
    bool ok = false;
    bool secure = false;
    std::wstring host;
    unsigned port = 443;
    std::wstring path;
};

// scheme://host[:port]/path. Rejects non-ASCII hosts (WinHttpConnect wants an
// ASCII hostname / punycode) and anything without a known scheme. A missing
// path defaults to "/".
ParsedUrl ParseHttpUrl(const std::string& url_utf8) {
    ParsedUrl out;
    std::string scheme;
    std::string rest;
    const std::size_t sep = url_utf8.find("://");
    if (sep == std::string::npos) return out;
    scheme = url_utf8.substr(0, sep);
    rest = url_utf8.substr(sep + 3);
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme == "https") {
        out.secure = true;
        out.port = 443;
    } else if (scheme == "http") {
        out.secure = false;
        out.port = 80;
    } else {
        return out;
    }
    const std::size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = WideFromUtf8(slash == std::string::npos ? "/" : rest.substr(slash));
    // Strip userinfo if present (rare, but harmless to handle).
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    if (authority.empty()) return out;
    // IPv6 literal?
    if (!authority.empty() && authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) return out;
        out.host = WideFromUtf8(authority.substr(0, close + 1));
        const std::size_t colon = authority.find(':', close);
        if (colon != std::string::npos) {
            const int port = std::atoi(authority.c_str() + colon + 1);
            if (port > 0 && port < 65536) out.port = static_cast<unsigned>(port);
        }
    } else {
        std::string host = authority;
        const std::size_t colon = host.rfind(':');
        if (colon != std::string::npos) {
            const int port = std::atoi(host.c_str() + colon + 1);
            if (port > 0 && port < 65536) out.port = static_cast<unsigned>(port);
            host = host.substr(0, colon);
        }
        out.host = WideFromUtf8(host);
    }
    // Hostname must be ASCII (punycode); non-ASCII would need IDN handling the
    // theme browser does not ship.
    for (const char c : authority) {
        if (static_cast<unsigned char>(c) > 0x7F) return ParsedUrl{};
    }
    if (out.host.empty()) return out;
    out.ok = true;
    return out;
}

}  // namespace

HttpBytesResult HttpGetBytes(const std::string& url_utf8, int timeout_ms) {
    HttpBytesResult out;
    const ULONGLONG t0 = GetTickCount64();
    const int timeout = timeout_ms > 0 ? timeout_ms : 15000;
    const ParsedUrl url = ParseHttpUrl(url_utf8);
    if (!url.ok) {
        out.error = ERROR_INVALID_PARAMETER;
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    HINTERNET session = WinHttpOpen(L"DockForge/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.error = GetLastError();
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }
    WinHttpSetTimeouts(session, 5000, timeout, timeout, timeout);

    HINTERNET connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    if (!connect) {
        out.error = GetLastError();
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", url.path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           url.secure ? WINHTTP_FLAG_SECURE : 0);
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
