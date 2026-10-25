// DockForge - Launchpad application index (CONTRACTS.md C-18).
//
// Launchpad shows every app the user can launch, which means merging three
// sources that know different things:
//
//   1. the shell AppsFolder - the same enumeration the Dock uses (C-08), so a
//      packaged app and a Win32 shortcut both appear with a usable label;
//   2. `launchpad.extra_paths` - arbitrary directories of shortcuts the user
//      pointed at, walked with shell::FindShortcuts;
//   3. `launchpad.folders[].items` - hand-curated identities, resolved against
//      whatever sources 1 and 2 produced.
//
// This module does the merge, the deduplication and the sort. It deliberately does
// NOT do its own shell enumeration: C-06 keeps shell parsing in start_menu /
// app_resolver, and a second enumeration path would drift from the Dock's.
//
// Why the index is a cache rather than a query: rebuilding walks 184 shortcuts on
// this machine plus the whole AppsFolder, which is far too slow for a per-frame
// call. The watcher exists so the cache can be invalidated without polling the
// filesystem on a timer.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/config.h"
#include "shell/app_entry.h"

namespace dockforge::shell {

// One grid entry: the app plus the identity string it is keyed by.
struct LaunchpadItem {
    AppEntry app;
    std::string identity;  // C-15a identity, lowercased
};

// One folder resolved against the current index.
//
// `missing` keeps the identities that did not resolve this time. They are NOT
// dropped from the config - the app may simply be uninstalled today and back
// tomorrow - but the grid should not show a hole where an icon used to be.
struct LaunchpadFolderView {
    std::string id;
    std::string name;
    std::vector<LaunchpadItem> items;
    std::vector<std::string> missing;
};

struct LaunchpadIndexStats {
    int apps_folder_count = 0;     // entries ScanAppsFolder returned to us
    int apps_folder_accepted = 0;  // of those, the ones the grid shows
    int extra_path_count = 0;      // extra_paths entries the grid shows (post-dedup)
    int duplicate_count = 0;       // dropped because another source already had it
    int skipped = 0;               // rejected here (documents, unresolvable shortcuts)
    int apps_folder_skipped = 0;   // rejected by ScanAppsFolder itself, never seen here
    int path_failures = 0;         // extra_paths that could not be walked
    double scan_ms = 0.0;

    // What the grid should contain.
    //
    // Both accepted counters are already post-dedup, so `duplicate_count` must NOT be
    // subtracted again - it is reported so the shortfall from the sources is
    // explainable, not because it is still in the sum. Measured with the machine-wide
    // Programs folder as an extra path: accepted=208, extra=72, dup=29, items=280, and
    // 208+72 is the answer while 208+72-29 is 29 short.
    //
    // `apps_folder_skipped` likewise stays out of the sum: it counts entries
    // ScanAppsFolder dropped before returning, which never reached this index (215
    // returned, 7 rejected here, 1 lost upstream, 208 shown).
    int ExpectedItemCount() const { return apps_folder_accepted + extra_path_count; }
};

// Sorts wide strings the way the Start menu does.
//
// Exposed for tests: the sort must use the user's collation (Chinese names and
// mixed case have to interleave the way a user expects), not byte order, and a
// byte-order sort would pass any test that only used ASCII names.
int CompareDisplayNames(const std::wstring& a, const std::wstring& b);

// Debounce gate for the filesystem watcher.
//
// Separated from the watcher thread because the timing rule is the part with the
// bug potential (a rebuild per file event would re-walk 184 shortcuts on every
// keystroke of a file copy) and this way it is unit tested without threads.
class DebounceGate {
public:
    explicit DebounceGate(double window_ms);

    void NoteChange(double now_ms);
    bool Consume(double now_ms);  // true exactly once per settled burst
    bool Pending() const { return pending_since_ms_ >= 0.0; }
    void Reset();

private:
    double window_ms_;
    double pending_since_ms_ = -1.0;
};

// Watches the directories the index was built from and reports settled changes.
//
// Uses ReadDirectoryChangesW rather than FindFirstChangeNotification because the
// former reports what changed, which lets a future increment avoid a full rescan.
// Today the result is only used as a "something changed" bit, but the plumbing is
// the expensive part and the filter flags are already correct.
//
// One thread serves every root through overlapped I/O and WaitForMultipleObjects,
// not one thread per root: a user can point `extra_paths` at 16 directories, and
// 18 blocking threads to notice a new shortcut is not a trade worth making.
class LaunchpadWatcher {
public:
    LaunchpadWatcher();
    ~LaunchpadWatcher();

    LaunchpadWatcher(const LaunchpadWatcher&) = delete;
    LaunchpadWatcher& operator=(const LaunchpadWatcher&) = delete;

    // Starts watching. Missing or unreadable roots are skipped with a WARN - a
    // Launchpad that refused to open because one extra path vanished would be
    // worse than one that stops noticing changes there. Returns false when no root
    // could be opened.
    bool Start(const std::vector<std::wstring>& roots, double debounce_ms = 500.0);

    // True at most once per settled burst. Safe to call every frame.
    bool ConsumeChanged(double now_ms);

    void Stop();
    bool Running() const { return impl_ != nullptr; }
    int WatchedCount() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// The directories whose changes should invalidate the index (C-18): the two Start
// menu Programs folders plus every `extra_paths` entry.
//
// The AppsFolder source is backed by these same two Programs folders on disk, so
// watching them covers a newly installed shortcut without polling the shell. Roots
// that do not exist are omitted here rather than passed on and warned about, since
// a machine with no per-user Start menu is ordinary, not an error.
std::vector<std::wstring> DefaultWatchRoots(const core::LaunchpadConfig& config);

// Builds the index from the three sources. COM must be initialized on this thread
// because the AppsFolder scan needs it (C-08).
class LaunchpadIndex {
public:
    // Full rebuild. `config` supplies extra_paths; folders are resolved lazily by
    // Folders() so that renaming a folder does not force a rescan.
    void Rebuild(const core::LaunchpadConfig& config);

    const std::vector<LaunchpadItem>& Items() const { return items_; }
    const LaunchpadIndexStats& Stats() const { return stats_; }
    bool Empty() const { return items_.empty(); }

    // Resolves `config.folders` against the current items. Identities that are not
    // in the index land in `missing` instead of being dropped from the config.
    std::vector<LaunchpadFolderView> Folders(const core::LaunchpadConfig& config) const;

    // Case-insensitive prefix filter over display names. Empty query returns
    // everything, which is what the grid wants before the user types.
    //
    // Prefix rather than substring (T-042 DoD 1): a Launchpad search that matched
    // the middle of a name would surface "Visual Studio Code" for "code", which is
    // rarely what the first keystrokes mean. Substring matching, if it ever comes,
    // has to be a distinct mode the user opts into rather than the default.
    std::vector<const LaunchpadItem*> Search(const std::wstring& query) const;

    // The live query filter used by the search field, as a set of indices into
    // Items(). Kept as indices rather than pointers so a concurrent rebuild can
    // invalidate the whole set without leaving dangling pointers in the caller.
    //
    // `match_mode` selects prefix (0) or substring (1). Substring is exposed for
    // the T-042 rename dialog's "find app" field, where the user is looking for
    // one app they already know and a prefix match over the whole Start menu is
    // the wrong default there.
    enum class MatchMode { Prefix = 0, Substring = 1 };
    std::vector<int> Filter(const std::wstring& query, MatchMode mode) const;

    // True when the two names match under `mode` with case folding. Exposed for
    // tests so the folding rule is asserted directly rather than inferred from
    // which apps happen to be installed.
    static bool MatchesQuery(const std::wstring& name, const std::wstring& query,
                             MatchMode mode);

private:
    std::vector<LaunchpadItem> items_;
    LaunchpadIndexStats stats_;
};

}  // namespace dockforge::shell
