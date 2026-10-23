// DockForge - application loop implementation.
//
// Frame pacing uses a high-resolution waitable timer rather than the timeout
// parameter of MsgWaitForMultipleObjectsEx. The plain timeout is quantised to
// the system timer tick (~15.6 ms by default), which caps the loop at ~31 ms
// per frame even when 16 ms was requested. With
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+) the deadline is
// honoured to ~0.5 ms, so 60 FPS is actually reachable.
#include "core/app_loop.h"

#include "core/log.h"
#include "platform/win_common.h"

#include <algorithm>
#include <cmath>

namespace dockforge::core {
namespace {

std::int64_t QpcFrequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value.QuadPart > 0 ? value.QuadPart : 1;
    }();
    return frequency;
}

std::int64_t MsToTicks(double milliseconds) {
    return static_cast<std::int64_t>(milliseconds * static_cast<double>(QpcFrequency()) / 1000.0);
}

}  // namespace

AppLoop::AppLoop() = default;

AppLoop::~AppLoop() {
    if (frame_timer_) {
        CloseHandle(frame_timer_);
        frame_timer_ = nullptr;
    }
}

bool AppLoop::EnsureFrameTimer() {
    if (frame_timer_) return true;

    // High-resolution variant exists from Windows 10 1803; on older builds the
    // call fails with ERROR_INVALID_PARAMETER and we retry without the flag.
    frame_timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!frame_timer_) {
        frame_timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        DF_WARN("loop", "high-resolution frame timer unavailable (err=%lu); pacing is coarse",
                GetLastError());
    }
    return frame_timer_ != nullptr;
}

void AppLoop::ArmFrameTimer(std::int64_t deadline_qpc) {
    if (!frame_timer_) return;

    const std::int64_t now = QueryPerformanceTicks();
    std::int64_t remaining_ticks = deadline_qpc - now;
    if (remaining_ticks < 1) remaining_ticks = 1;

    // SetWaitableTimer takes a relative due time in 100 ns units, negative for
    // "relative". Round up so we never wake before the deadline and spin.
    const std::int64_t freq = QpcFrequency();
    std::int64_t due_100ns = (remaining_ticks * 10'000'000LL + freq - 1) / freq;
    if (due_100ns < 1) due_100ns = 1;

    LARGE_INTEGER due{};
    due.QuadPart = -due_100ns;
    if (!SetWaitableTimer(frame_timer_, &due, 0, nullptr, nullptr, FALSE)) {
        DF_WARN("loop", "SetWaitableTimer failed (err=%lu)", GetLastError());
    }
}

void AppLoop::SetTargetIntervalMs(int interval_ms) {
    interval_ms_ = std::clamp(interval_ms, 1, 1000);
    next_deadline_qpc_ = 0;  // recompute against the new cadence
}

void AppLoop::Quit(int exit_code) {
    exit_code_ = exit_code;
    running_ = false;
    PostQuitMessage(exit_code);
}

void AppLoop::DispatchDueMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            exit_code_ = static_cast<int>(msg.wParam);
            running_ = false;
            return;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void AppLoop::PumpFrame() {
    const std::int64_t now = QueryPerformanceTicks();
    if (last_stats_qpc_ == 0) last_stats_qpc_ = now;

    if (tick_) {
        // The first frame after a stall reports the real delta so the tick
        // handler can decide to skip animations rather than jump.
        const double delta = clock_.Tick();
        tick_(delta);
    }

    if (stats_ && now - last_stats_qpc_ >= QpcFrequency()) {
        last_stats_qpc_ = now;
        stats_(clock_);
    }
}

int AppLoop::Run() {
    running_ = true;
    exit_code_ = 0;
    next_deadline_qpc_ = 0;
    last_stats_qpc_ = 0;
    EnsureFrameTimer();

    while (running_) {
        DispatchDueMessages();
        if (!running_) break;

        const std::int64_t now = QueryPerformanceTicks();
        if (next_deadline_qpc_ == 0) next_deadline_qpc_ = now;

        if (now >= next_deadline_qpc_) {
            PumpFrame();
            idle_this_frame_ = false;
            // Advance on a fixed cadence so timer jitter does not accumulate
            // into drift. If we fell more than one interval behind (a stall),
            // resync to now instead of firing a burst of catch-up frames.
            const std::int64_t interval_ticks = MsToTicks(interval_ms_);
            next_deadline_qpc_ += interval_ticks;
            if (next_deadline_qpc_ <= now) {
                next_deadline_qpc_ = now + interval_ticks;
            }
            continue;
        }

        // Block until either input arrives or the frame timer fires.
        ArmFrameTimer(next_deadline_qpc_);
        HANDLE handles[1] = {frame_timer_};
        const DWORD count = frame_timer_ ? 1 : 0;
        const DWORD timeout =
            frame_timer_ ? INFINITE
                         : static_cast<DWORD>(std::clamp(
                               QpcToMilliseconds(next_deadline_qpc_ - now), 1.0, 1000.0));
        const DWORD result =
            MsgWaitForMultipleObjectsEx(count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_FAILED) {
            DF_ERROR("loop", "MsgWaitForMultipleObjectsEx failed (err=%lu)", GetLastError());
            break;
        }
        idle_this_frame_ = (result == WAIT_TIMEOUT || (count == 1 && result == WAIT_OBJECT_0));
    }

    return exit_code_;
}

}  // namespace dockforge::core
