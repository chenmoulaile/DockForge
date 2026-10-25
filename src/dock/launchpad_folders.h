// DockForge - Launchpad folder authoring (CONTRACTS.md C-18, T-042).
//
// Creating a folder by dragging one icon onto another, and renaming one, both
// mutate `launchpad.folders` in the config and then persist it. That is the only
// place in the Launchpad that writes to disk, and it is deliberately isolated
// here rather than spread across the view so the rules have one home:
//
//   * an id is a v4 UUID, generated on creation, never derived from the name;
//   * renaming changes the name only - the id is what the grid rearranges by,
//     and a rename that changed the id would lose the folder mid-drag;
//   * appending an app to an existing folder is idempotent, so a double drop
//     cannot produce a duplicate entry;
//   * every mutation goes through ClampConfig, so a hand-edited folder count
//     above the limit is trimmed rather than rejected;
//   * a mutation that would make the config invalid is refused, not partially
//     applied.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"

namespace dockforge::dock {

// A v4 UUID in lowercase canonical form, e.g. "3f2a1c4e-9b7d-4f6a-8c5e-1d2b3a4c5d6e".
//
// Generated from CoCreateGuid, not from a counter: a counter would collide after
// a config round-trip on another machine (the same folder re-imported from a
// backup would get a different id than the one the user's grid remembers).
std::string NewFolderId();

// Adds a new folder holding `items` and returns its id. An empty `items` list is
// refused: a folder with nothing in it is a hole in the grid, and C-18 already
// says such folders are discarded on load, so creating one would produce a
// config entry that immediately vanishes.
std::string CreateLaunchpadFolder(core::Config& config, const std::string& name,
                                  const std::vector<std::string>& items);

// Renames an existing folder. Returns false when the id is unknown, which is the
// caller's cue to re-read the config: another window may have edited it.
bool RenameLaunchpadFolder(core::Config& config, const std::string& id,
                           const std::string& name);

// Appends `identity` to a folder, ignoring an append that is already present.
// Returns false when the id is unknown or the folder is at the item limit.
bool AppendToLaunchpadFolder(core::Config& config, const std::string& id,
                             const std::string& identity);

// Removes `identity` from a folder. A folder that becomes empty is removed
// entirely rather than left as a zero-item entry C-18 would discard on load.
// Returns false when the id is unknown or the identity was not in the folder.
bool RemoveFromLaunchpadFolder(core::Config& config, const std::string& id,
                               const std::string& identity);

// Deletes a folder outright. Returns false when the id is unknown.
bool DeleteLaunchpadFolder(core::Config& config, const std::string& id);

// True when `identity` is already a member of folder `id`.
bool LaunchpadFolderContains(const core::Config& config, const std::string& id,
                             const std::string& identity);

// Finds a folder by id, returning nullptr when absent. The returned pointer is
// into `config` and is invalidated by any mutation, so callers must not hold it
// across a call that writes.
const core::LaunchpadFolder* FindLaunchpadFolder(const core::Config& config,
                                                 const std::string& id);

// Serialises and writes the config. Kept as one function so the write path is
// auditable in one place and so the Launchpad cannot forget the temp-file dance
// core::SaveConfig performs. Returns false when the save failed, in which case
// the caller must NOT treat the in-memory mutation as committed.
bool SaveLaunchpadConfig(const core::Config& config);

}  // namespace dockforge::dock
