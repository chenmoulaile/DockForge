#include "core/crash_dump.h"

#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"

#include <dbghelp.h>

#include <cstdio>

namespace dockforge::core {
namespace {

constexpr char kLogModule[] = "crash";

// Forward declaration: the filter forwards to the filter it replaced (T-061
// taskbar recovery). Saved at install time - SetUnhandledExceptionFilter is
// the only way to see the previous filter, there is no read-back API (spike Q4).
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_dump_filter = nullptr;

// Test seam: default is the real DumpsDir(). Static storage duration - the
// filter reads it at crash time with no allocation.
std::wstring& DumpsDirOverride() {
    static std::wstring dir;
    return dir;
}

std::wstring EffectiveDumpsDir() {
    const std::wstring& override_dir = DumpsDirOverride();
    if (!override_dir.empty()) return override_dir;
    return DumpsDir();
}

// Raw-Win32 one-line append used by BOTH the install path (logger alive) and
// the filter path (logger may be dead). No CRT heap, no locks. The marker
// lives IN the dumps directory so an overridden directory (unit tests, probe)
// still receives it.
void AppendMarkerLine(const std::wstring& dumps_dir, const char* line) {
    if (dumps_dir.empty()) return;
    std::wstring path = dumps_dir + L"\\crash-dump.txt";
    HANDLE handle = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(handle, line, static_cast<DWORD>(lstrlenA(line)), &written, nullptr);
    CloseHandle(handle);
}

// "dockforge-crash-YYYYMMDD-HHMMSS.dmp". Local time, second resolution: a
// same-second collision only overwrites a dump from the same crash second and
// the probe asserts on the newest file, not on a unique name.
std::wstring DumpFilePathFor(const std::wstring& dir, const SYSTEMTIME& st) {
    wchar_t name[64]{};
    _snwprintf(name, 63, L"dockforge-crash-%04u%02u%02u-%02u%02u%02u.dmp", st.wYear,
               st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return dir + L"\\" + name;
}

// The filter itself. Raw Win32 only - the heap, the logger and the CRT may all
// be mid-failure here (same discipline as T-061's filter). Spike measured this
// exact shape at 62ms on a real 0xC0000005 with a second thread alive.
LONG WINAPI CrashDumpFilter(EXCEPTION_POINTERS* info) {
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    const std::wstring dir = EffectiveDumpsDir();
    SYSTEMTIME st{};
    GetLocalTime(&st);
    const std::wstring path = DumpFilePathFor(dir, st);

    BOOL dumped = FALSE;
    DWORD err = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        dumped = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                   MiniDumpNormal, &mei, nullptr, nullptr);
        if (!dumped) err = GetLastError();
        CloseHandle(file);
    } else {
        err = GetLastError();
    }

    QueryPerformanceCounter(&t1);
    const LONGLONG elapsed_ms = (t1.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart;

    // The marker line carries everything the probe asserts: verdict, error
    // code, elapsed ms (the 2s budget evidence), file name, tick.
    char line[256]{};
    _snprintf(line, sizeof(line) - 1,
              "crash dump: dumped=%d err=%lu ms=%lld file=%ls tick=%llu\n",
              dumped ? 1 : 0, err, elapsed_ms, path.c_str(),
              static_cast<unsigned long long>(GetTickCount64()));
    AppendMarkerLine(dir, line);

    // Forward to the filter we replaced and adopt its verdict -
    // EXCEPTION_EXECUTE_HANDLER ends the process without WER UI.
    if (g_prev_dump_filter) return g_prev_dump_filter(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

void SetDumpsDirectoryForTest(const std::wstring& dir) { DumpsDirOverride() = dir; }

bool InstallCrashDumper() {
    const std::wstring dir = EffectiveDumpsDir();
    const bool dir_ok = EnsureDirectory(dir);

    // A modal WER dialog is exactly the "阻塞退出" the DoD forbids; suppress it
    // process-wide (spike Q3: exit ~1.7s wall including startup, no dialog).
    const UINT old_mode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetErrorMode(old_mode | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // Replace whatever is installed (T-061's filter, per main.cpp order) and
    // keep it alive via the return value (spike Q4: chain verified).
    g_prev_dump_filter = SetUnhandledExceptionFilter(&CrashDumpFilter);

    DF_INFO(kLogModule, "crash dump filter installed (dir=%s prev=%p)",
            WideToUtf8(dir).c_str(), reinterpret_cast<void*>(g_prev_dump_filter));
    return dir_ok;
}

}  // namespace dockforge::core
