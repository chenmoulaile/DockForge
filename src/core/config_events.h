// DockForge - event payloads on the core event bus (CONTRACTS.md C-20).
#pragma once

#include "core/config.h"

namespace dockforge::core {

// Published after the settings window (or any future writer) persisted a config
// change. C-20 freezes the payload shape as a COMPLETE snapshot, not a key/value
// diff: every subscriber recomputes layout from it, and a partial update would
// make each of them merge keys on their own. The config object is ~1KB, so the
// copy is free next to the work the subscribers do with it.
struct ConfigChanged {
    core::Config config;
};

// Published by the taskbar module wiring (T-061) whenever Shell_TrayWnd's
// observed visibility flips, so consumers can react without polling the shell
// window (C-20 table). The payload is the observed state, not the requested
// one: ShowWindowAsync settles asynchronously.
struct TaskbarVisibilityChanged {
    bool visible;
};

}  // namespace dockforge::core
