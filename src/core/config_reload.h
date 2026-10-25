// DockForge - hot-reload decision for config.json (T-051, CONTRACTS.md C-20).
//
// The pure half of config hot reload: given the config the app currently runs
// with, decide what to do with what is on disk. Kept free of watchers and
// windows so every rule is unit-testable:
//
//   * invalid JSON is REJECTED and the running config survives - a half-written
//     file (the writer had not flushed when our debounce fired) must never be
//     mistaken for a real edit, and must never rename the user's file;
//   * a file whose content serialises back to the same text as `current` is
//     UNCHANGED - this is what stops the reload loop after our own save (the
//     settings window saves, the watcher sees config.json change, reload would
//     re-publish the same snapshot forever without this branch);
//   * out-of-range values are clamped and adopted, matching what the loader
//     does on startup, so "disk truth" is the same on both paths;
//   * a missing file is UNCHANGED, not an error: a deleted config is not a
//     reload trigger, and re-creating defaults here would fight the owner.
#pragma once

#include <string>

#include "core/config.h"

namespace dockforge::core {

enum class ReloadVerdict {
    // The on-disk config parsed and differed from `current`; `next` holds it,
    // already clamped. The caller publishes ConfigChanged with it.
    Adopted,
    // Nothing to do: the file is byte-equivalent in content, or missing.
    Unchanged,
    // The file exists but is not a valid config. `current` must be kept and the
    // reason logged. The file itself is left untouched on disk.
    Rejected,
};

ReloadVerdict ReloadConfigFromDisk(const core::Config& current, core::Config& next,
                                   std::string* reason);

}  // namespace dockforge::core
