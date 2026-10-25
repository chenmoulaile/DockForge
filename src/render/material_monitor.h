// DockForge - the single material-tier poller (CONTRACTS.md C-21).
//
// C-21 forbids every window from reading the power state on its own: if the
// dock, the hardware panel and the notification banner each polled, they would
// degrade on different frames and the desktop would look torn. So exactly one
// object polls, and it publishes MaterialDegraded / MaterialRestored on the bus.
//
// Two blocking conditions, both from C-21:
//   * battery-saver mode on (SYSTEM_POWER_STATUS.SystemStatusFlag == 1);
//   * the render device lost, reported by the composition surface, which is the
//     only component that can actually see it.
// The capture-unavailable case is decided once at startup by ProbeMaterialCaps
// and never changes while the process runs, so it stays out of the monitor -
// re-probing it every 5s would be a screen DC round-trip for an answer that
// cannot move.
#pragma once

#include "core/config.h"
#include "core/event_bus.h"
#include "render/material.h"

#include <cstdint>
#include <functional>

namespace dockforge::render {

// What the monitor reads. Injectable so the state machine is testable without a
// battery: the real implementation is one GetSystemPowerStatus call.
struct PowerSnapshot {
    bool battery_saver = false;  // SystemStatusFlag == 1
    bool on_battery = false;     // ACLineStatus == 0 (informational, not a trigger)
};

using PowerProbe = std::function<PowerSnapshot()>;

// Reads the machine's real power state.
PowerSnapshot ReadSystemPower();

// The published tier the monitor degrades from and restores to.
struct MaterialBaseline {
    core::MaterialLevel requested = core::MaterialLevel::Mica;  // what the user configured
    core::MaterialLevel resolved = core::MaterialLevel::Mica;   // what this machine can render
};

class MaterialMonitor {
  public:
    // `poll_interval_ms` is C-21's 5s; exposed so a test can drive it faster
    // without sleeping.
    MaterialMonitor(core::EventBus& bus, PowerProbe probe, double poll_interval_ms = 5000.0);

    // Set once at startup from ProbeMaterialCaps + ResolveMaterialTier.
    void SetBaseline(const MaterialBaseline& baseline);
    const MaterialBaseline& baseline() const { return baseline_; }

    // The tier everything should draw with right now.
    core::MaterialLevel current_tier() const { return degraded_ ? degraded_to_ : baseline_.resolved; }
    bool degraded() const { return degraded_; }

    // Advances the poll timer and republishes on any change. Safe to call every
    // frame: it only touches the probe when the interval elapses.
    void Tick(double delta_ms);

    // The render backend's device-lost signal. `lost` true degrades to Solid
    // immediately (no waiting for the next poll - a lost device means nothing
    // renders at all); false re-arms the restore path.
    void SetDeviceLost(bool lost);

    // Forces one poll now, ignoring the interval. Used by the probe channel so
    // an evidence run does not have to wait 5s per step.
    void PollNow();

  private:
    void Evaluate(bool device_lost);

    core::EventBus& bus_;
    PowerProbe probe_;
    double poll_interval_ms_;
    double since_poll_ms_ = 1e9;  // poll on the first tick
    MaterialBaseline baseline_{};
    bool degraded_ = false;
    core::MaterialLevel degraded_to_ = core::MaterialLevel::Solid;
    bool device_lost_ = false;
};

}  // namespace dockforge::render
