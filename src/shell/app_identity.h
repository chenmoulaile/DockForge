// DockForge - the app identity string (CONTRACTS.md C-15a).
//
// Identity answers one question: when are two AppEntry values "the same app"?
// The Dock uses it to key `dock.tiles` / `dock.hidden`; Launchpad uses it to
// deduplicate its three sources and to key `launchpad.folders[].items`.
//
// It lives in src/shell rather than src/dock because both modules need it and
// C-06 forbids shell from depending on dock. There is exactly one implementation:
// a second one would silently diverge the first time the precedence changes, which
// is the failure C-14f already documents for window matching.
#pragma once

#include <string>

#include "shell/app_entry.h"

namespace dockforge::shell {

// ASCII-only lowercasing.
//
// Deliberately not towlower: identity strings are compared byte-wise, and locale
// folding would make a Turkish locale produce different keys for the same app than
// a US one, so a config written on one machine would orphan tiles on another.
std::string LowerAscii(std::string text);

// Stable, lowercased identity for an app.
//
// Precedence, in order: AUMID, shell-namespace parsing name, filesystem path,
// display name. The display name is last because two different apps can share one,
// and it is locale-dependent - but it is still better than no identity at all,
// which would make a tile impossible to reorder or hide.
std::string IdentityOf(const AppEntry& app);

}  // namespace dockforge::shell
