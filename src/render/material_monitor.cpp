// DockForge - the single material-tier poller (see material_monitor.h).
#include "render/material_monitor.h"

#include "core/log.h"
#include "render/material_events.h"

namespace dockforge::render {
namespace {

// C-21's ladder: mica -> acrylic -> blur -> solid, down only. Stepping one rung
// per evaluation (rather than jumping to Solid) is what the ordering is FOR: a
// dock that snaps from a system backdrop to a flat plate in one frame reads as a
// glitch, while three 5s steps read as the machine conserving power. The final
// rung is always renderable, which is what makes it a safe place to land.
core::MaterialLevel NextLowerTier(core::MaterialLevel level) {
    switch (level) {
        case core::MaterialLevel::Mica: return core::MaterialLevel::Acrylic;
        case core::MaterialLevel::Acrylic: return core::MaterialLevel::Blur;
        case core::MaterialLevel::Blur: return core::MaterialLevel::Solid;
        case core::MaterialLevel::Solid: return core::MaterialLevel::Solid;
    }
    return core::MaterialLevel::Solid;
}

}  // namespace

PowerSnapshot ReadSystemPower() {
    PowerSnapshot snapshot;
    SYSTEM_POWER_STATUS status{};
    if (!GetSystemPowerStatus(&status)) return snapshot;
    snapshot.battery_saver = status.SystemStatusFlag == 1;
    snapshot.on_battery = status.ACLineStatus == 0;
    return snapshot;
}

MaterialMonitor::MaterialMonitor(core::EventBus& bus, PowerProbe probe, double poll_interval_ms)
    : bus_(bus), probe_(std::move(probe)), poll_interval_ms_(poll_interval_ms) {}

void MaterialMonitor::SetBaseline(const MaterialBaseline& baseline) {
    baseline_ = baseline;
    if (!degraded_) degraded_to_ = baseline.resolved;
}

void MaterialMonitor::Tick(double delta_ms) {
    since_poll_ms_ += delta_ms;
    if (since_poll_ms_ < poll_interval_ms_) return;
    since_poll_ms_ = 0.0;
    Evaluate(device_lost_);
}

void MaterialMonitor::SetDeviceLost(bool lost) {
    if (lost == device_lost_) return;
    device_lost_ = lost;
    // Both directions react immediately: waiting up to 5s to notice a lost
    // device means up to 5s of a window that cannot draw, and waiting to notice
    // a recovered one means a dock that stays flat for no visible reason.
    Evaluate(lost);
}

void MaterialMonitor::PollNow() {
    since_poll_ms_ = 0.0;
    Evaluate(device_lost_);
}

void MaterialMonitor::Evaluate(bool device_lost) {
    const PowerSnapshot power = probe_ ? probe_() : PowerSnapshot{};

    if (device_lost) {
        // A lost device cannot composite anything, so there is no "next rung" to
        // try: the ladder is skipped and Solid is the only honest answer.
        if (degraded_ && degraded_to_ == core::MaterialLevel::Solid) return;
        const core::MaterialLevel from = degraded_ ? degraded_to_ : baseline_.resolved;
        degraded_ = true;
        degraded_to_ = core::MaterialLevel::Solid;
        if (from == core::MaterialLevel::Solid) return;
        DF_WARN("render", "material degraded %s -> solid: render device lost", core::ToString(from));
        bus_.Publish(core::MaterialDegraded{from, core::MaterialLevel::Solid, "render device lost"});
        return;
    }

    if (power.battery_saver) {
        const core::MaterialLevel from =
            degraded_ ? degraded_to_ : baseline_.resolved;
        if (from == core::MaterialLevel::Solid) {
            // Already at the bottom rung; stay degraded but stop publishing.
            degraded_ = true;
            degraded_to_ = core::MaterialLevel::Solid;
            return;
        }
        const core::MaterialLevel to = NextLowerTier(from);
        degraded_ = true;
        degraded_to_ = to;
        DF_WARN("render", "material degraded %s -> %s: battery saver mode is on",
                core::ToString(from), core::ToString(to));
        bus_.Publish(core::MaterialDegraded{from, to, "battery saver mode is on"});
        return;
    }

    if (!degraded_) return;
    degraded_ = false;
    degraded_to_ = baseline_.resolved;
    // One step back to the tier the user configured (already clamped by what the
    // machine can render). Climbing the ladder rung by rung would show acrylic
    // for one poll interval on the way back to a Mica the user asked for.
    DF_INFO("render", "material restored %s", core::ToString(baseline_.resolved));
    bus_.Publish(core::MaterialRestored{baseline_.resolved});
}

}  // namespace dockforge::render
