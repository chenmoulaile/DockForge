// T-060 system-level probe binary: drive the real Task Scheduler through the
// production autostart module (core/autostart.cpp, COM ITaskService) so a
// PowerShell driver can cross-check the outcome with schtasks /query.
//
//   autostart_probe.exe on    -> ApplyAutostartSystem(true)
//   autostart_probe.exe off   -> ApplyAutostartSystem(false)
//   autostart_probe.exe exists-> query only, exit 0 when present
//
// Exit codes: 0 = action/query succeeded, 1 = failed, 2 = bad usage. Output is
// one machine-parsable line: RESULT <ok|failed> <detail>.
#include "core/autostart.h"

#include <windows.h>

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("RESULT failed usage: autostart_probe.exe on|off|exists\n");
        return 2;
    }
    const std::string verb = argv[1];

    if (verb == "exists") {
        const bool present = dockforge::core::AutostartTaskExists();
        std::printf("RESULT %s %s\n", present ? "ok" : "failed",
                    present ? "task present" : "task absent");
        return present ? 0 : 1;
    }

    if (verb != "on" && verb != "off") {
        std::printf("RESULT failed unknown verb %s\n", verb.c_str());
        return 2;
    }

    std::string detail;
    const bool ok =
        dockforge::core::ApplyAutostartSystem(verb == "on", &detail);
    std::printf("RESULT %s %s\n", ok ? "ok" : "failed", detail.c_str());
    return ok ? 0 : 1;
}
