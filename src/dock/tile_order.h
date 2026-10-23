// DockForge - curated tile order (T-013, CONTRACTS.md C-01 dock.tiles / dock.hidden).
//
// The dock's tile list has two sources of truth that must not be confused: the
// AppsFolder scan (what exists) and the config (what the user wants to see, in what
// order). This file is the seam between them - the only place that knows how an app
// becomes a stable string and how those strings rebuild a tile list.
//
// Why an identity string rather than an index: an index is meaningless the moment an
// app is installed or uninstalled, and the scan order is not guaranteed stable across
// shell updates. A string survives both, at the cost of having to decide what makes
// two entries "the same app" - which is the AUMID-or-path rule already frozen in
// C-13c for window matching.
#pragma once

#include "core/config.h"
#include "dock/dock_view.h"
#include "shell/app_entry.h"

#include <string>
#include <vector>

namespace dockforge::dock {

// Stable, lowercased identity for an app, used as the key in `dock.tiles` and
// `dock.hidden`.
//
// Order matters and matches C-13c: AUMID first, then the shell-namespace parsing
// name, then the filesystem path. Falling back to the display name would make the
// order depend on the UI language, so it is used only when nothing else exists - a
// last resort that keeps a tile from being unaddressable.
std::string IdentityOf(const shell::AppEntry& app);

// Rebuilds the tile list from a scan plus the curated order.
//
// Two modes, decided by whether `order` is empty:
//
//   * `order` empty - unmanaged. The dock auto-populates from the scan, which is
//     what a first run looks like and what makes the product usable before anyone
//     has arranged anything.
//   * `order` non-empty - curated. The order is authoritative: named entries that
//     still exist are shown, in that order, and NOTHING is appended. Auto-appending
//     would make removal invisible - the user drags a tile off the dock and a
//     different app immediately takes the slot, which reads as "removal did not
//     work". Getting new apps onto a curated dock is an add gesture, not a
//     side-effect of deletion.
//
// A named entry that no longer exists is skipped silently: apps get uninstalled,
// and a dock that refused to start over a stale entry would be worse than one that
// quietly forgets it.
std::vector<shell::AppEntry> ApplyOrder(const std::vector<shell::AppEntry>& apps,
                                        const std::vector<std::string>& order,
                                        const std::vector<std::string>& hidden,
                                        int max_tiles);

// Moves one tile and leaves the rest in order. `to` is the index the tile occupies
// afterwards, which is what DragController reports.
void MoveTile(std::vector<DockTile>& tiles, int from, int to);

// Erases one tile. Returns false when the index is out of range.
bool RemoveTile(std::vector<DockTile>& tiles, int index);

// Writes the current visible order into `config.dock.tiles` and records removals in
// `config.dock.hidden`.
//
// Called on every reorder and removal rather than on exit: a crash between the
// gesture and the save would otherwise lose the arrangement the user just made, and
// there is no way to reconstruct it from the scan.
void CommitOrder(core::Config& config, const std::vector<DockTile>& tiles,
                 const std::string& removed_identity);

}  // namespace dockforge::dock
