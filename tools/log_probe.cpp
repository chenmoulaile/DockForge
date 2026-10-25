// DockForge T-062 log-rotation probe host.
//
// tools/log_probe.ps1 drives this console tool to exercise the REAL product
// Logger (src/core/log.cpp - the same code DockForge.exe runs) against a
// scratch directory, which the full GUI app cannot offer: it would take real
// megabytes and real days to hit the limits. Every knob is the same
// environment variable the app honours:
//   DOCKFORGE_LOG_DIR        scratch logs directory (paths.cpp isolation seam)
//   DOCKFORGE_LOG_MAX_BYTES  size cap, e.g. 20000
//   DOCKFORGE_LOG_KEEP_DAYS  age window, e.g. 1
//
// Usage:
//   log_probe.exe spin <approx_bytes>   Init, write ~bytes through DF_INFO,
//                                       Shutdown, print the file inventory
//   log_probe.exe once <message>        Init, one INFO line, Shutdown
//
// The tool never touches the real profile: without DOCKFORGE_LOG_DIR set it
// refuses to run (the ps1 always sets it).
#include <windows.h>

#include "core/log.h"
#include "core/paths.h"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: log_probe.exe spin <approx_bytes> | once <message>\n");
        return 2;
    }
    if (GetEnvironmentVariableW(L"DOCKFORGE_LOG_DIR", nullptr, 0) == 0) {
        std::printf("refusing to run without DOCKFORGE_LOG_DIR (isolation)\n");
        return 2;
    }

    const std::string command = argv[1];
    auto& logger = dockforge::core::Logger::Instance();
    logger.Init(L"logprobe");

    if (command == "once") {
        DF_INFO("logprobe", "once: %s", argc > 2 ? argv[2] : "");
    } else if (command == "spin") {
        const long long target = argc > 2 ? atoll(argv[2]) : 100000;
        // ~80-char payload per line; the formatted line is ~140 bytes total.
        const std::string payload(80, 'x');
        for (long long written = 0; written < target; written += 140) {
            DF_INFO("logprobe", "spin line %lld %s", written, payload.c_str());
        }
    } else {
        std::printf("unknown command: %s\n", command.c_str());
        logger.Shutdown();
        return 2;
    }

    // Inventory for the driver: path of every file the run produced.
    const std::wstring dir = dockforge::core::LogsDir();
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((dir + L"\\dockforge-*.log").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::printf("file %ls %llu\n", data.cFileName,
                        (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
                            data.nFileSizeLow);
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    std::printf("logprobe done dir=%ls\n", dir.c_str());
    logger.Shutdown();
    return 0;
}
