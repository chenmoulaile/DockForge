// DockForge - which tiles are running, and which one is in front (T-012).
//
// Two different clocks drive this module, and keeping them apart is the whole
// design:
//
//   * Window *existence* changes slowly and costs a full EnumWindows sweep
//     (~0.4-0.7ms measured in evidence/T-011), so it is polled on an interval -
//     300ms by default. Faster buys nothing: an app that opens and closes inside
//     300ms is a flash, not a state.
//   * Window *foreground* changes on every alt-tab and costs one GetForegroundWindow
//     call (sub-microsecond), so it is read on every frame. Throttling it to the
//     poll interval would make the highlight visibly lag the window it describes.
//
// A single bool would collapse these into one flag and force the slow clock onto
// both, which is exactly the trade this module exists to avoid.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "dock/dock_view.h"
#include "shell/app_entry.h"
#include "shell/window_registry.h"

namespace dockforge::dock {

class RunningTracker {
  public:
    // 300ms per T-012. Clamped to 50..5000 so a bad value cannot turn the dock into
    // a busy loop or a frozen indicator.
    explicit RunningTracker(int interval_ms = 300);

    void SetIntervalMs(int interval_ms);
    int IntervalMs() const { return interval_ms_; }

    // Advances the poll clock by `delta_ms` and re-enumerates when due.
    //
    // Returns true when the visible state may have changed - the window set
    // changed, or the foreground window did. The caller uses that to repaint, so a
    // false return means the dock can stay asleep.
    bool Update(double delta_ms);

    // State of one app against the latest snapshot.
    //
    // Linear over the snapshot, which is fine because the caller only runs it when
    // Update returned true: 12 tiles x ~200 windows every 300ms is well under a
    // millisecond, and making it a hash index would mean keeping the index in sync
    // with a container that is rebuilt wholesale anyway.
    TileRunState StateFor(const shell::AppEntry& app) const;

    // Recomputes `run_state` on every tile. Only call after Update returned true.
    void ApplyToTiles(std::vector<DockTile>& tiles) const;

    const std::vector<shell::AppWindow>& Windows() const { return windows_; }
    HWND Foreground() const { return foreground_; }

    // Forces the next Update to re-enumerate. Used after a launch so the new
    // window's dot appears without waiting out the interval.
    void Invalidate() { next_poll_ms_ = 0.0; }

    struct Stats {
        std::uint64_t polls = 0;
        std::uint64_t foreground_changes = 0;
        double last_poll_ms = 0.0;
        double total_poll_ms = 0.0;
        double max_poll_ms = 0.0;
        double AveragePollMs() const {
            return polls ? total_poll_ms / static_cast<double>(polls) : 0.0;
        }
    };
    const Stats& GetStats() const { return stats_; }

  private:
    int interval_ms_;
    double since_poll_ms_ = 0.0;
    // Time until the next sweep. Zero means "due now".
    double next_poll_ms_ = 0.0;

    std::vector<shell::AppWindow> windows_;
    HWND foreground_ = nullptr;
    Stats stats_{};
};

}  // namespace dockforge::dock
