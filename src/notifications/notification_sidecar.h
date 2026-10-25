// DockForge - .NET sidecar bridge for the notification centre (T-076).
//
// Why a child process: UserNotificationListener's entry points are
// IAsyncOperation, and those never complete in this process. T-065 measured it
// in a console process; T-076 re-measured it with RoInitialize(STA) plus a real
// PeekMessage/DispatchMessage pump for 8s and the status stayed Started the whole
// time (evidence/T-076/spike_async_pump.log). The .NET projection completes the
// same call in ~0.7s, so the listener lives in tools/notification_sidecar.exe and
// we speak one-JSON-object-per-line over pipes, exactly like T-068's hw_sidecar.
//
// The transport is platform::LineProcess (shared with the media bridge), so this
// class holds only the protocol: what to send, and how to fold the answers.
#pragma once

#include "notifications/notification_types.h"
#include "platform/line_process.h"
#include "platform/win_common.h"

#include <string>
#include <vector>

namespace dockforge::notifications {

// Everything one drain of the pipe produced. Folded from complete lines only; a
// half-written line stays in the transport buffer.
struct NotificationDrain {
    bool ready = false;                            // the sidecar announced itself
    AccessStatus ready_access = AccessStatus::Unspecified;
    bool pushed = false;                           // at least one push line
    std::vector<NotificationState> states;         // every poll answer, in order
    std::wstring error;                            // last protocol error, else ""
    int lines = 0;                                 // complete lines consumed
};

// Folds lines into `drain`. Pure - no process, no clock - so the whole protocol
// can be tested from string literals. Unparsable lines are counted and dropped.
void FoldNotificationLines(const std::vector<std::string>& lines, NotificationDrain* drain);

class NotificationSidecarBridge {
  public:
    NotificationSidecarBridge() = default;
    ~NotificationSidecarBridge();

    NotificationSidecarBridge(const NotificationSidecarBridge&) = delete;
    NotificationSidecarBridge& operator=(const NotificationSidecarBridge&) = delete;

    // Spawns the sidecar. `exe_path` explicit so a probe can point at a stub;
    // empty means "next to the app binary". False when it could not start, in
    // which case the centre reports itself unavailable instead of empty.
    bool Start(const std::wstring& exe_path = L"");

    // Closes the pipes and reaps the child. Safe to call twice.
    void Stop();

    bool running() const { return process_.running(); }

    // The three commands the sidecar understands.
    bool RequestPoll();
    bool RequestClear();
    bool RemoveItem(std::uint32_t id);

    // Non-blocking read of everything currently in the pipe.
    NotificationDrain DrainEvents();

    bool ChildDied() { return process_.ChildDied(); }
    unsigned long pid() const { return process_.pid(); }

  private:
    platform::LineProcess process_;
};

}  // namespace dockforge::notifications
