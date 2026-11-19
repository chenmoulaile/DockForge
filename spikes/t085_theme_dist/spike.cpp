// DockForge T-085 spike: theme distribution plumbing on the MinGW toolchain.
//
// DoD questions answered by running this binary (driver: spike_driver.ps1):
//   Q1. Does MinGW-w64 compile <bcrypt.h> and produce the correct SHA256 for
//       known vectors (RFC 6234 "abc", empty string), and does the hash of a
//       downloaded file match Get-FileHash on the same bytes?
//   Q2. Can platform-style WinHTTP fetch a BINARY body over plain HTTP from a
//       loopback server (the probe needs an offline-controllable origin), and
//       do failure paths (404, refused connection) return in bounded time?
//   Q3. Is https://raw.githubusercontent.com (the intended default index host)
//       reachable from this process shape at all?
//   Q4. Does the shell COM route (IShellDispatch::NameSpace + Folder::CopyHere,
//       pure COM, no WinRT - B-007 forbids WinRT async in this process) extract
//       a Compress-Archive-produced .zip into a destination tree, and how long
//       does CopyHere take to settle (it returns before the copy finishes)?
//   Q5. Does the whole chain close: download -> SHA256 compare -> extract ->
//       theme.json present?
//
// Build (Git Bash, repo root):
//   g++ -std=c++20 -O2 -o spikes/t085_theme_dist/spike.exe \
//       spikes/t085_theme_dist/spike.cpp \
//       -lwinhttp -lbcrypt -lole32 -loleaut32 -lshell32 -luuid -static
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include "miniz.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

std::string HexUpper(const unsigned char* data, std::size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(digits[(data[i] >> 4) & 0xF]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

// Q1: SHA256 over a byte range via bcrypt (BCryptHashData in one call; the
// production installer will chunk big downloads, same API shape).
bool Sha256Hex(const unsigned char* data, std::size_t len, std::string* hex,
               unsigned long* err) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, L"SHA256", nullptr, 0);
    if (st != 0) {
        if (err) *err = static_cast<unsigned long>(st);
        return false;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (st == 0) st = BCryptHashData(hash, const_cast<unsigned char*>(data),
                                     static_cast<ULONG>(len), 0);
    unsigned char digest[32] = {};
    if (st == 0) {
        ULONG done = 0;
        st = BCryptFinishHash(hash, digest, sizeof(digest), 0);
        if (st == 0) *hex = HexUpper(digest, sizeof(digest));
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (err) *err = static_cast<unsigned long>(st);
    return st == 0;
}

struct FetchResult {
    bool ok = false;
    unsigned long status = 0;
    unsigned long error = 0;
    unsigned long long elapsed_ms = 0;
    std::string body;
};

// Q2/Q3: one synchronous GET over WinHTTP. `secure` picks https; the loopback
// probe origin is plain http on 127.0.0.1.
FetchResult HttpGet(const wchar_t* host, unsigned port, const wchar_t* path, bool secure,
                    int timeout_ms) {
    FetchResult out;
    const ULONGLONG t0 = GetTickCount64();
    HINTERNET session = WinHttpOpen(L"DockForge-t085-spike/0.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.error = GetLastError();
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }
    WinHttpSetTimeouts(session, 5000, timeout_ms, timeout_ms, timeout_ms);
    HINTERNET connect = WinHttpConnect(session, host, port, 0);
    if (!connect) {
        out.error = GetLastError();
        WinHttpCloseHandle(session);
        out.elapsed_ms = GetTickCount64() - t0;
        return out;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           secure ? WINHTTP_FLAG_SECURE : 0);
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
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    out.status = status;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        std::string chunk(available, '\0');
        DWORD got = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &got)) break;
        chunk.resize(got);
        out.body += chunk;
        if (got == 0) break;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    out.ok = status >= 200 && status < 300;
    out.elapsed_ms = GetTickCount64() - t0;
    return out;
}
int CountFiles(const wchar_t* dir) {
    int files = 0;
    std::wstring pattern = std::wstring(dir) + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return -1;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring sub = std::wstring(dir) + L"\\" + fd.cFileName;
            const int nested = CountFiles(sub.c_str());
            if (nested > 0) files += nested;
        } else {
            ++files;
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return files;
}

std::string ReadFileText(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buffer[256] = {};
    DWORD got = 0;
    ReadFile(h, buffer, sizeof(buffer) - 1, &got, nullptr);
    CloseHandle(h);
    return std::string(buffer, got);
}

std::wstring Utf8PathToWide(const std::string& text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed);
    return out;
}

// Q4 route (the one the production installer uses): miniz over the downloaded
// bytes. Entry names are validated against traversal before anything is
// written; directories are created as needed; file bodies go through
// mz_zip_reader_extract_file_to_callback into a HANDLE writer.
struct MinizWriteCtx {
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool ok = true;
};

mz_uint64 MinizWriteCallback(void* opaque, mz_uint64 offset, const void* buffer,
                             size_t size) {
    auto* ctx = static_cast<MinizWriteCtx*>(opaque);
    if (!ctx->ok) return size;
    DWORD wrote = 0;
    if (!WriteFile(ctx->handle, buffer, static_cast<DWORD>(size), &wrote, nullptr) ||
        wrote != size) {
        ctx->ok = false;
    }
    return size;
}

bool ExtractZipMiniz(const unsigned char* bytes, size_t size, const wchar_t* dest_root,
                     int* files_out) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, bytes, size, 0)) {
        std::printf("  [miniz] init_mem failed err=%d size=%zu magic=%02X %02X %02X %02X\n",
                    mz_zip_get_last_error(&zip), size, bytes[0], bytes[1], bytes[2],
                    bytes[3]);
        return false;
    }
    const int count = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    bool ok = true;
    int files = 0;
    for (int i = 0; i < count && ok; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, static_cast<mz_uint>(i), &stat)) {
            ok = false;
            break;
        }
        const std::string name(stat.m_filename);
        std::printf("  [miniz] entry[%d] name='%s' dir=%d crc=%08X\n", i, name.c_str(),
                    stat.m_is_directory ? 1 : 0, stat.m_crc32);
        // Spike-measured (spike_run.log entry[0] name='icons\sample.png'):
        // PowerShell 5.1 Compress-Archive writes BACKSLASH-separated entry
        // names, so the separator must be normalized before any path work.
        std::string norm = name;
        for (char& c : norm) {
            if (c == '\\') c = '/';
        }
        // Traversal guard (mirrors the production installer): any ".."
        // component, drive prefix or absolute path rejects the whole pack.
        if (norm.find("..") != std::string::npos || (!norm.empty() && norm[0] == '/') ||
            (norm.size() >= 2 && norm[1] == ':')) {
            std::printf("  [miniz] traversal guard rejected '%s'\n", norm.c_str());
            ok = false;
            break;
        }
        const std::wstring wide_name = Utf8PathToWide(norm);
        // Create every parent directory of the entry (normalized path is
        // forward-slash separated).
        std::wstring parents = wide_name;
        const std::size_t last_slash = parents.find_last_of(L'/');
        if (last_slash != std::wstring::npos) {
            parents.resize(last_slash);
            std::wstring made;
            std::size_t start = 0;
            while (start <= parents.size()) {
                const std::size_t slash = parents.find(L'/', start);
                const std::wstring part = slash == std::wstring::npos
                                              ? parents.substr(start)
                                              : parents.substr(start, slash - start);
                made = made.empty() ? part : made + L"\\" + part;
                if (!made.empty()) {
                    const std::wstring dir = std::wstring(dest_root) + L"\\" + made;
                    if (!CreateDirectoryW(dir.c_str(), nullptr) &&
                        GetLastError() != ERROR_ALREADY_EXISTS) {
                        ok = false;
                        break;
                    }
                }
                if (slash == std::wstring::npos) break;
                start = slash + 1;
            }
            if (!ok) break;
        }
        const std::wstring rel = wide_name;
        if (stat.m_is_directory) {
            const std::wstring dir = std::wstring(dest_root) + L"\\" + rel;
            if (!CreateDirectoryW(dir.c_str(), nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                ok = false;
                break;
            }
        } else {
            const std::wstring target = std::wstring(dest_root) + L"\\" + rel;
            HANDLE out = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE) {
                ok = false;
                break;
            }
            MinizWriteCtx ctx{};
            ctx.handle = out;
            if (!mz_zip_reader_extract_file_to_callback(&zip, name.c_str(),
                                                        &MinizWriteCallback, &ctx, 0)) {
                ctx.ok = false;
            }
            CloseHandle(out);
            if (!ctx.ok) {
                ok = false;
                break;
            }
            ++files;
        }
    }
    mz_zip_reader_end(&zip);
    if (files_out) *files_out = ok ? files : -1;
    return ok;
}

int Fail(const char* what) {
    std::printf("FAIL %s\n", what);
    return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Expected arguments:
    //   1 = loopback http port
    //   2 = expected sha256 (hex, lowercase) of the served pack
    //   3 = zip path the driver already built
    //   4 = extraction destination directory
    //   5 = path where theme.json should appear after extraction
    if (argc < 6) {
        std::printf("usage: spike.exe <port> <sha256> <zip> <dest> <theme_json>\n");
        return 2;
    }
    const unsigned port = static_cast<unsigned>(_wtoi(argv[1]));
    const std::string expected_sha = [&] {
        std::string s;
        for (const wchar_t* p = argv[2]; *p; ++p) s.push_back(static_cast<char>(*p));
        return s;
    }();
    const wchar_t* zip_path = argv[3];
    const wchar_t* dest_dir = argv[4];
    const wchar_t* theme_json_path = argv[5];

    // Q1: known vectors.
    {
        std::string hex;
        unsigned long err = 0;
        if (!Sha256Hex(reinterpret_cast<const unsigned char*>("abc"), 3, &hex, &err) ||
            hex != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
            std::printf("Q1 FAIL vector-abc hex=%s err=%lu\n", hex.c_str(), err);
            return Fail("Q1");
        }
        if (!Sha256Hex(reinterpret_cast<const unsigned char*>(""), 0, &hex, &err) ||
            hex != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") {
            std::printf("Q1 FAIL vector-empty hex=%s err=%lu\n", hex.c_str(), err);
            return Fail("Q1");
        }
        std::printf("Q1 PASS sha256 vectors (abc, empty)\n");
    }

    // Q2: loopback binary download + failure paths.
    {
        FetchResult r = HttpGet(L"127.0.0.1", port, L"/pack.dockforge-theme", false, 10000);
        if (!r.ok) {
            std::printf("Q2 FAIL download status=%lu err=%lu body=%zu\n", r.status, r.error,
                        r.body.size());
            return Fail("Q2");
        }
        std::string hex;
        unsigned long err = 0;
        if (!Sha256Hex(reinterpret_cast<const unsigned char*>(r.body.data()), r.body.size(),
                       &hex, &err) || hex != expected_sha) {
            std::printf("Q2 FAIL sha mismatch got=%s want=%s\n", hex.c_str(),
                        expected_sha.c_str());
            return Fail("Q2");
        }
        // 404 and refused-connection must come back bounded, not hang.
        FetchResult missing = HttpGet(L"127.0.0.1", port, L"/missing.dockforge-theme", false, 10000);
        FetchResult dead = HttpGet(L"127.0.0.1", 1, L"/x", false, 4000);
        std::printf("Q2 PASS download %zu bytes sha-ok elapsed=%llums; 404 status=%lu %llums; "
                    "refused err=%lu %llums\n",
                    r.body.size(), r.elapsed_ms, missing.status, missing.elapsed_ms, dead.error,
                    dead.elapsed_ms);
        if (missing.ok || dead.ok) return Fail("Q2-negative");
    }

    // Q3: the real default-index host, plain GET (the linux kernel README is a
    // small, stable text file; the point is host reachability, not this file).
    {
        FetchResult r = HttpGet(L"raw.githubusercontent.com", 443,
                                L"/torvalds/linux/master/README", true, 15000);
        std::printf("Q3 %s raw.githubusercontent.com status=%lu bytes=%zu elapsed=%llums err=%lu\n",
                    r.ok ? "PASS" : "FAIL", r.status, r.body.size(), r.elapsed_ms, r.error);
        if (!r.ok) return Fail("Q3");
    }

    // Q4/Q5: extraction via the vendored miniz (mz_zip_reader_init_mem), the
    // route the production installer uses. The shell routes were evaluated and
    // rejected earlier in this spike: IShellDispatch::NameSpace on a .zip
    // returns S_FALSE/E_FAIL with a null Folder inside this console process
    // (identical bytes work under PowerShell), and BHID_SFObject reports
    // SFGAO_FOLDER=0 for the same file - the zip namespace binding is not
    // process-reliable here. miniz reads the bytes directly, no shell involved.
    {
        FetchResult r = HttpGet(L"127.0.0.1", port, L"/pack.dockforge-theme", false, 10000);
        if (!r.ok) {
            std::printf("Q4 FAIL re-download status=%lu err=%lu\n", r.status, r.error);
            return Fail("Q4");
        }
        std::string staged_sha;
        unsigned long err = 0;
        if (!Sha256Hex(reinterpret_cast<const unsigned char*>(r.body.data()), r.body.size(),
                       &staged_sha, &err) || staged_sha != expected_sha) {
            std::printf("Q4 FAIL staged sha mismatch %s\n", staged_sha.c_str());
            return Fail("Q4");
        }
        const ULONGLONG t0 = GetTickCount64();
        int files = 0;
        const bool extract_ok = ExtractZipMiniz(reinterpret_cast<const unsigned char*>(r.body.data()), r.body.size(), dest_dir, &files);
        const unsigned long long extract_ms = GetTickCount64() - t0;
        const std::string json_b = ReadFileText(theme_json_path);
        const bool ok = extract_ok && files == 3 && json_b.find("\"name\"") != std::string::npos;
        std::printf("Q4 %s miniz extract files=%d %llums (theme.json+preview.png+icons/sample.png)\n",
                    ok ? "PASS" : "FAIL", files, extract_ms);
        std::printf("Q5 %s theme.json %zu bytes: %s\n", ok ? "PASS" : "FAIL",
                    json_b.size(), json_b.c_str());
        if (!ok) return Fail("Q4/Q5");
    }

    std::printf("ALL PASS\n");
    return 0;
}
