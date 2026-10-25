// DockForge T-062 spike: MiniDumpWriteDump under MinGW-w64, measured.
//
// Questions the T-062 implementation depends on (the toolchain note claims
// "mingw 头部有 dbghelp" - claims are verified here, never assumed):
//   Q1  Does g++ (MSYS2 15.2.0) link MiniDumpWriteDump from -ldbghelp and does
//       the declared MINIDUMP_TYPE / exception-info surface compile clean?
//   Q2  Inside a SetUnhandledExceptionFilter, does MiniDumpWriteDump produce a
//       loadable .dmp, and how long does it take? (DoD budget: 2 seconds)
//   Q3  Does the process exit promptly after the filter chain returns (no hang),
//       and what is the exit code?
//   Q4  Does the two-filter chain work: the dump filter installed LAST receives
//       the exception FIRST, writes the dump, then forwards to the previously
//       installed filter (T-061's taskbar recovery) - does the forwarded filter
//       run and does the chain's return value come from the tail?
//   Q5  With a second (sleeping) worker thread alive, does the dump still get
//       written from the crashing thread? (the dock always has D2D/DComp
//       internal threads; MiniDumpWriteDump walks all of them)
//
// Build (same toolchain as the repo, see tools/gate.ps1 PATH):
//   g++ -std=c++20 -O2 -Wall -o spike.exe spike.cpp -ldbghelp
// Run (from the directory that should receive spike-crash-*.dmp):
//   spike.exe
// The spike crashes ON PURPOSE; its console output plus the .dmp file are the
// findings. A wrapper script records the exit code and timings.

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>

namespace {

// Q4 tail: stands in for T-061's taskbar recovery filter (raw Win32, no heap).
LONG WINAPI TaskbarStyleFilter(EXCEPTION_POINTERS*) {
    HANDLE marker = CreateFileW(L"spike-marker.txt", FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE) {
        const char line[] = "tail filter ran: taskbar-restore step executed\n";
        DWORD written = 0;
        WriteFile(marker, line, sizeof(line) - 1, &written, nullptr);
        CloseHandle(marker);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Q4 head: the T-062 dump filter shape. The previous filter is saved AT INSTALL
// time (SetUnhandledExceptionFilter's return value), not discovered at crash
// time - there is no Win32 API to read it back.
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

LONG WINAPI DumpFilter(EXCEPTION_POINTERS* info) {
    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    wchar_t path[MAX_PATH]{};
    _snwprintf(path, MAX_PATH - 1, L"spike-crash-%lu.dmp", GetCurrentProcessId());

    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    BOOL dumped = FALSE;
    DWORD err = 0;
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

    char line[192]{};
    _snprintf(line, sizeof(line) - 1, "head filter: dumped=%d err=%lu ms=%lld file=%ls\n",
              dumped ? 1 : 0, err, elapsed_ms, path);
    printf("%s", line);
    fflush(stdout);
    HANDLE marker = CreateFileW(L"spike-marker.txt", FILE_APPEND_DATA, FILE_SHARE_READ,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (marker != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(marker, line, static_cast<DWORD>(lstrlenA(line)), &written, nullptr);
        CloseHandle(marker);
    }

    if (g_prev_filter) return g_prev_filter(info);
    return EXCEPTION_EXECUTE_HANDLER;
}

DWORD WINAPI WorkerThread(LPVOID) {
    // Q5: a thread alive (but not holding our locks) while the main thread dies.
    // MiniDumpWriteDump must freeze-and-walk it without hanging.
    for (;;) Sleep(1000);
    return 0;
}

}  // namespace

int main() {
    printf("[spike] pid=%lu\n", GetCurrentProcessId());

    // Q5 worker thread: alive at crash time.
    HANDLE worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (worker) CloseHandle(worker);

    // Suppress WER UI: a modal crash dialog would be exactly the "阻塞退出" the
    // DoD forbids. The real install path does the same.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // Install order mirrors main.cpp: taskbar (T-061) first, dump (T-062) last,
    // so the dump filter runs first and forwards.
    SetUnhandledExceptionFilter(&TaskbarStyleFilter);
    g_prev_filter = SetUnhandledExceptionFilter(&DumpFilter);
    printf("[spike] previous filter=%p (TaskbarStyleFilter=%p) chain_installed=%d\n",
           reinterpret_cast<void*>(g_prev_filter),
           reinterpret_cast<void*>(&TaskbarStyleFilter),
           g_prev_filter == &TaskbarStyleFilter ? 1 : 0);
    fflush(stdout);

    printf("[spike] arming null dereference\n");
    fflush(stdout);
    Sleep(300);

    int* poison = nullptr;
    *poison = 42;  // 0xC0000005

    // Unreachable on a working filter chain.
    printf("[spike] ERROR: survived the crash\n");
    return 0;
}
