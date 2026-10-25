// Deterministic stand-in for tools/notification_sidecar.cs, used only by
// tools/notifications_probe.ps1 -StubSidecar.
//
// Why it exists: the real clear path calls UserNotificationListener.ClearNotifications,
// which wipes the machine's actual Windows notification centre. That is fine for a
// user who clicks 清除, but it is not something an automated evidence run should do
// to the operator's machine. The stub speaks the identical line protocol, so the
// dock-side clear chain (probe command -> store.ClearLocal -> sidecar "clear" ->
// next poll answers empty -> panel renders empty) is exercised end to end without
// touching any real notification.
//
// Protocol, one JSON object per line, same as the .NET sidecar:
//   <- {"ok":true,"sidecar":"ready","pid":N,"access":1}
//   -> poll                      <- {"ok":true,"access":1,"count":N,"items":[...]}
//   -> clear                     <- {"ok":true}
//   -> remove <id>               <- {"ok":true}
//   -> quit                      (exit 0)
//   -> anything else             <- {"ok":false,"error":"bad command"}
//
// Build:
//   g++ -std=c++17 -O2 -o build\bin\notification_sidecar_stub.exe stub_sidecar.cpp -static

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct StubItem {
    unsigned int id;
    const char* aumid;
    const char* app;
    const char* title;
    const char* body;
};

// Two fixed rows. The ids are stable so the dock's seen-set behaves exactly as it
// would against real toasts (no re-announcing on every poll).
std::vector<StubItem> MakeItems() {
    return {
        {0x1001u, "Stub.Probe.One", "Stub One", "stub title one", "stub body one"},
        {0x1002u, "Stub.Probe.Two", "Stub Two", "stub title two", "stub body two"},
    };
}

void EmitItems(const std::vector<StubItem>& items) {
    std::printf("{\"ok\":true,\"access\":1,\"count\":%zu,\"items\":[", items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) std::printf(",");
        const StubItem& item = items[i];
        std::printf("{\"id\":%u,\"aumid\":\"%s\",\"app\":\"%s\",\"title\":\"%s\",\"body\":\"%s\","
                    "\"created\":%lld}",
                    item.id, item.aumid, item.app, item.title, item.body,
                    static_cast<long long>(1700000000));
    }
    std::printf("]}\n");
    std::fflush(stdout);
}

}  // namespace

int main() {
    std::printf("{\"ok\":true,\"sidecar\":\"ready\",\"pid\":%lu,\"access\":1}\n",
                static_cast<unsigned long>(GetCurrentProcessId()));
    std::fflush(stdout);

    std::vector<StubItem> items = MakeItems();
    char line[512];
    while (std::fgets(line, sizeof(line), stdin) != nullptr) {
        std::string command(line);
        while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) {
            command.pop_back();
        }
        if (command == "poll") {
            EmitItems(items);
        } else if (command == "clear") {
            items.clear();
            std::printf("{\"ok\":true}\n");
            std::fflush(stdout);
        } else if (command.rfind("remove ", 0) == 0) {
            const unsigned int id = static_cast<unsigned int>(std::strtoul(command.c_str() + 7, nullptr, 10));
            std::vector<StubItem> kept;
            kept.reserve(items.size());
            for (const StubItem& item : items) {
                if (item.id != id) kept.push_back(item);
            }
            items.swap(kept);
            std::printf("{\"ok\":true}\n");
            std::fflush(stdout);
        } else if (command == "quit") {
            return 0;
        } else if (!command.empty()) {
            std::printf("{\"ok\":false,\"error\":\"bad command\"}\n");
            std::fflush(stdout);
        }
    }
    // stdin closed: the parent is gone, so leave.
    return 0;
}
