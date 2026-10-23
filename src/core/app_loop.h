// DockForge - application loop: message pump + frame timer.
//
// The loop blocks in MsgWaitForMultipleObjectsEx with a timeout equal to the
// time left until the next frame deadline, so an idle Dock burns no CPU while
// still ticking at the requested rate. Animation work hangs off the tick
// callback and is expected to finish well inside one interval.
#pragma once

#include "core/frame_clock.h"

#include <cstdint>
#include <functional>

namespace dockforge::core {

class AppLoop {
  public:
    // Called once per frame with the measured delta in milliseconds.
    using TickHandler = std::function<void(double delta_ms)>;
    // Called once per second with the rolling stats (used for PERF logging).
    using StatsHandler = std::function<void(const FrameClock&)>;

    AppLoop();
    ~AppLoop();

    void SetTickHandler(TickHandler handler) { tick_ = std::move(handler); }
    void SetStatsHandler(StatsHandler handler) { stats_ = std::move(handler); }

    // Frame interval in milliseconds. 16 ms targets ~60 FPS (PERFORMANCE.md).
    void SetTargetIntervalMs(int interval_ms);
    int TargetIntervalMs() const { return interval_ms_; }

    // Runs until Quit() is called. Returns the exit code passed to Quit().
    int Run();
    void Quit(int exit_code = 0);

    FrameClock& Clock() { return clock_; }
    const FrameClock& Clock() const { return clock_; }

    // True when no message arrived and the loop had to wait out the interval.
    // Sustained true means the app is fully idle.
    bool IdleThisFrame() const { return idle_this_frame_; }

  private:
    void DispatchDueMessages();
    void PumpFrame();
    bool EnsureFrameTimer();
    void ArmFrameTimer(std::int64_t deadline_qpc);

    TickHandler tick_;
    StatsHandler stats_;
    FrameClock clock_{1000};
    int interval_ms_ = 16;
    bool running_ = false;
    bool idle_this_frame_ = false;
    int exit_code_ = 0;
    std::int64_t next_deadline_qpc_ = 0;
    std::int64_t last_stats_qpc_ = 0;
    // High-resolution waitable timer used for frame pacing (see app_loop.cpp).
    void* frame_timer_ = nullptr;
};

}  // namespace dockforge::core
