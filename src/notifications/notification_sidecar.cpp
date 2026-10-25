// DockForge - notification sidecar bridge (see notification_sidecar.h).
#include "notifications/notification_sidecar.h"

#include "core/log.h"
#include "core/paths.h"

namespace dockforge::notifications {

void FoldNotificationLines(const std::vector<std::string>& lines, NotificationDrain* drain) {
    if (drain == nullptr) return;
    for (const std::string& line : lines) {
        if (line.empty()) continue;
        NotificationState state;
        bool is_push = false;
        bool ready = false;
        std::wstring error;
        if (!ParseNotificationLine(line, &state, &is_push, &ready, &error)) {
            DF_DEBUG("notifications", "dropped unparsable sidecar line (%zu bytes)", line.size());
            continue;
        }
        ++drain->lines;
        if (is_push) {
            drain->pushed = true;
            continue;
        }
        if (ready) {
            drain->ready = true;
            drain->ready_access = state.access;
            continue;
        }
        if (!error.empty()) {
            drain->error = std::move(error);
            continue;
        }
        if (state.valid) drain->states.push_back(std::move(state));
    }
}

NotificationSidecarBridge::~NotificationSidecarBridge() { Stop(); }

bool NotificationSidecarBridge::Start(const std::wstring& exe_path) {
    const std::wstring path =
        platform::SiblingExecutablePath(exe_path, L"notification_sidecar.exe");
    if (path.empty()) return false;
    if (!process_.Start(path)) {
        DF_WARN("notifications", "notification_sidecar not started (missing or spawn failed): %s",
                core::WideToUtf8(path).c_str());
        return false;
    }
    DF_INFO("notifications", "notification_sidecar started pid=%lu", process_.pid());
    return true;
}

void NotificationSidecarBridge::Stop() { process_.Stop(); }

bool NotificationSidecarBridge::RequestPoll() { return process_.SendLine("poll"); }

bool NotificationSidecarBridge::RequestClear() { return process_.SendLine("clear"); }

bool NotificationSidecarBridge::RemoveItem(std::uint32_t id) {
    return process_.SendLine("remove " + std::to_string(id));
}

NotificationDrain NotificationSidecarBridge::DrainEvents() {
    NotificationDrain drain;
    FoldNotificationLines(process_.DrainLines(), &drain);
    return drain;
}

}  // namespace dockforge::notifications
