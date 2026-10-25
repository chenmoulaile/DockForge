// DockForge - material degradation events (CONTRACTS.md C-21).
//
// The contract exists because of a specific visual bug: if every window decides
// on its own when to drop from Mica to Solid, the dock, the hardware panel and
// the notification banner degrade on different frames and the desktop looks
// torn for as long as it takes each one to notice. So exactly one poller exists
// (MaterialMonitor) and every material-using window subscribes here.
//
// "Degrade only, never upgrade on its own": a degraded tier stays degraded until
// the monitor says every blocking condition cleared, at which point
// MaterialRestored carries the tier the user actually configured - not one step
// up the ladder. Anything else would creep back to Mica one flicker at a time.
#pragma once

#include "core/config.h"

namespace dockforge::core {

// `reason` is a string literal with static storage duration (the contract's
// payload type), so a subscriber may keep the pointer past the dispatch.
struct MaterialDegraded {
    core::MaterialLevel from = core::MaterialLevel::Solid;
    core::MaterialLevel to = core::MaterialLevel::Solid;
    const char* reason = "";
};

struct MaterialRestored {
    core::MaterialLevel to = core::MaterialLevel::Solid;
};

}  // namespace dockforge::core
