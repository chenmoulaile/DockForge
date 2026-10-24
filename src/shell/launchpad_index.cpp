// DockForge - Launchpad index, watcher and folder resolution (C-18).
#include "shell/launchpad_index.h"

#include <windows.h>
#include <shlobj.h>

#include "core/log.h"
#include "core/paths.h"
#include "platform/win_common.h"
#include "shell/app_identity.h"
#include "shell/app_resolver.h"
#include "shell/start_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <unordered_set>

namespace dockforge::shell {
namespace {

std::wstring LowerWide(std::wstring text) {
    for (wchar_t& ch : text) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return text;
}

// Compares two wide strings with the user's collation.
//
// SORT_DEFAULT keeps the linguistic rules (so "Z" and "Ä" land where a reader
// expects), and NORM_LINGUISTIC_CASING is what makes "abc" and "ABC" tie instead of
// sorting every capital ahead of every lowercase. Without the casing flag an
// all-caps app name sorts to the top of the grid, which is not how the Start menu
// behaves and not what a user would call alphabetical.
int CompareNames(const std::wstring& a, const std::wstring& b) {
    const int result = ::CompareStringEx(LOCALE_NAME_USER_DEFAULT, SORT_DEFAULT | NORM_LINGUISTIC_CASING,
                                         a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                         static_cast<int>(b.size()), nullptr, nullptr, 0);
    if (result == CSTR_LESS_THAN) return -1;
    if (result == CSTR_GREATER_THAN) return 1;
    if (result == CSTR_EQUAL) return 0;
    // CompareStringEx returns 0 on failure (bad locale, OOM). Falling back to a
    // byte comparison keeps the grid stable instead of leaving it in whatever order
    // the scan produced.
    const int fallback = a.compare(b);
    return fallback < 0 ? -1 : (fallback > 0 ? 1 : 0);
}

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - start).count();
}

bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix) {
    const std::size_t len = std::wcslen(suffix);
    if (text.size() < len) return false;
    return ::_wcsicmp(text.c_str() + (text.size() - len), suffix) == 0;
}

// Is this something you launch, or something you open?
//
// The AppsFolder scan already yields only launchable entries, but an
// `extra_paths` shortcut can point at a document: the WinRAR Start-menu folder on
// this machine ships one that resolves to `winrar\whatsnew.txt`, and without this
// filter it appears in the grid as a tile that opens a readme. Measured: 296 items
// before the filter, 295 after.
//
// The rule is deliberately about what can be launched, not about extensions being
// "app-like": `regedit.exe`, `charmap.exe` and `comexp.msc` are all legitimate
// entries in the real Start menu and must survive.
//
// Both `Shortcut` and `NamespaceItem` need the trailing extension, because both
// carry only a path. `AppEntry::launchable()` is not enough here: it answers "can I
// start this", and ShellExecute can start a .txt - which is exactly how a document
// reaches the grid. Measured on a .lnk to readme.txt: resolve=none, kind=Shortcut,
// launchable=1.
bool EndsWithLaunchableExtension(const std::wstring& path) {
    return EndsWithNoCase(path, L".exe") || EndsWithNoCase(path, L".msc") ||
           EndsWithNoCase(path, L".cpl") || EndsWithNoCase(path, L".bat") ||
           EndsWithNoCase(path, L".cmd") || EndsWithNoCase(path, L".com");
}

bool LooksLikeApplication(const AppEntry& app) {
    switch (app.kind) {
        case AppKind::Uwp:
        case AppKind::ShellItem:
            // No path at all; the AUMID is the identity and the launch target.
            return !app.aumid.empty();
        case AppKind::Executable:
            return true;
        case AppKind::Shortcut:
            // The .lnk itself is never the launch target; its resolution is. A
            // shortcut with no target would not have resolved, so reaching here
            // with an empty path means the resolver gave us nothing usable.
            return !app.target_path.empty() && EndsWithLaunchableExtension(app.target_path);
        case AppKind::NamespaceItem:
            // "{GUID}\thing.ext" - only the trailing name is available, so the
            // extension is the only signal. .msc and .cpl are control panels, which
            // the Start menu lists as apps.
            return EndsWithLaunchableExtension(app.namespace_parsing);
        case AppKind::Folder:
            // A filesystem folder is not an app. Launchpad folders are a different
            // concept (curated identity lists in launchpad.folders).
            return false;
        case AppKind::Unknown:
            return false;
    }
    return false;
}

// Where a directory watcher should point for a configured extra path.
//
// The config stores UTF-8; the filesystem wants UTF-16. A path that does not exist
// is returned as-is so Start() can report it once, in one place.
std::wstring ToWidePath(const std::string& utf8) { return core::Utf8ToWide(utf8); }

}  // namespace

int CompareDisplayNames(const std::wstring& a, const std::wstring& b) { return CompareNames(a, b); }

namespace {

// Folder names are UTF-8 in the config but must sort with the same collation the
// grid uses, so they are widened once here rather than compared as bytes.
int CompareUtf8Names(const std::string& a, const std::string& b) {
    return CompareNames(core::Utf8ToWide(a), core::Utf8ToWide(b));
}

}  // namespace

// ------------------------------------------------------------------ DebounceGate

DebounceGate::DebounceGate(double window_ms) : window_ms_(window_ms) {}

void DebounceGate::NoteChange(double now_ms) {
    // Every event moves the deadline. A 184-file copy would otherwise trigger one
    // rebuild per file.
    pending_since_ms_ = now_ms;
}

bool DebounceGate::Consume(double now_ms) {
    if (pending_since_ms_ < 0.0) return false;
    if (now_ms - pending_since_ms_ < window_ms_) return false;
    pending_since_ms_ = -1.0;
    return true;
}

void DebounceGate::Reset() { pending_since_ms_ = -1.0; }

// --------------------------------------------------------------- LaunchpadWatcher

struct LaunchpadWatcher::Impl {
    struct Watch {
        HANDLE directory = INVALID_HANDLE_VALUE;
        // A real OVERLAPPED, not an event handle cast to LPOVERLAPPED. With
        // FILE_FLAG_OVERLAPPED the kernel writes the byte count and status into
        // this structure, so it must be a genuine OVERLAPPED that stays alive and
        // at a stable address for the whole operation - passing a HANDLE here
        // would have the kernel write into the handle's numeric value.
        OVERLAPPED overlapped{};
        // The buffer must also outlive the operation. A local scratch buffer in
        // the loop would be reused by the next ReadDirectoryChangesW while the
        // previous one is still pending, so each watch owns its own.
        std::vector<std::uint8_t> buffer;

        ~Watch() {
            if (directory != INVALID_HANDLE_VALUE) {
                // CancelIoEx before closing: a pending overlapped read holds a
                // reference to the directory, and closing it first leaves the
                // kernel writing into a buffer that is about to be freed.
                ::CancelIoEx(directory, &overlapped);
                ::CloseHandle(directory);
            }
            if (overlapped.hEvent != nullptr) ::CloseHandle(overlapped.hEvent);
        }
    };

    // Held by unique_ptr, never by value. A Watch armed with an in-flight
    // ReadDirectoryChangesW must not move: the kernel holds the address of its
    // OVERLAPPED, and a vector reallocation would leave it writing into freed
    // memory. Measured the hard way - the first version stored Watch by value and
    // pushed it into the vector after arming, so the watch never fired at all.
    std::vector<std::unique_ptr<Watch>> watches;
    HANDLE stop_event = nullptr;
    HANDLE thread = nullptr;
    DebounceGate gate{500.0};

    // Set from the watcher thread, consumed on the UI thread. `changed` is a plain
    // bool guarded by the debounce gate, not by a lock: the watcher only ever
    // raises it and the consumer only ever clears it after the gate says the burst
    // settled, so a lost raise can only mean one extra rebuild.
    std::atomic<bool> changed{false};

    static unsigned long __stdcall ThreadMain(void* self) {
        static_cast<Impl*>(self)->ThreadLoop();
        return 0;
    }

    void ThreadLoop() {
        std::vector<HANDLE> wait_handles;
        wait_handles.reserve(watches.size() + 1);
        wait_handles.push_back(stop_event);
        for (const auto& watch : watches) wait_handles.push_back(watch->overlapped.hEvent);

        for (;;) {
            const DWORD count = static_cast<DWORD>(wait_handles.size());
            const DWORD index =
                ::WaitForMultipleObjects(count, wait_handles.data(), FALSE, INFINITE);
            if (index == WAIT_OBJECT_0) return;  // stop_event
            if (index == WAIT_FAILED || index < WAIT_OBJECT_0 || index >= WAIT_OBJECT_0 + count) {
                return;
            }
            const std::size_t which = index - WAIT_OBJECT_0 - 1;
            if (which >= watches.size()) return;

            Watch& watch = *watches[which];
            DWORD returned = 0;
            const BOOL ok = ::GetOverlappedResult(watch.directory, &watch.overlapped, &returned,
                                                  FALSE);
            ::ResetEvent(watch.overlapped.hEvent);
            // Re-arm regardless of `ok`: a failed read means the directory went
            // away, and re-arming will fail again harmlessly while the UI keeps
            // working off the last good index.
            ::ReadDirectoryChangesW(watch.directory, watch.buffer.data(),
                                    static_cast<DWORD>(watch.buffer.size()), TRUE,
                                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                        FILE_NOTIFY_CHANGE_LAST_WRITE,
                                    nullptr, &watch.overlapped, nullptr);
            // A zero-byte success means the buffer overflowed: the tree changed
            // more than 64KB worth of names at once. The consumer does a full
            // rebuild anyway, so the truncated list is never interpreted.
            if (ok) changed.store(true, std::memory_order_relaxed);
        }
    }

    void Stop() {
        if (stop_event != nullptr) ::SetEvent(stop_event);
        if (thread != nullptr) {
            ::WaitForSingleObject(thread, 2000);
            ::CloseHandle(thread);
            thread = nullptr;
        }
        // Watches release their own handles in ~Watch, which cancels the pending
        // overlapped read first.
        watches.clear();
        if (stop_event != nullptr) {
            ::CloseHandle(stop_event);
            stop_event = nullptr;
        }
    }
};

LaunchpadWatcher::LaunchpadWatcher() = default;

LaunchpadWatcher::~LaunchpadWatcher() { Stop(); }

bool LaunchpadWatcher::Start(const std::vector<std::wstring>& roots, double debounce_ms) {
    Stop();
    impl_ = new Impl();
    impl_->gate = DebounceGate(debounce_ms);
    impl_->stop_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl_->stop_event == nullptr) {
        DF_WARN("shell", "launchpad watcher: CreateEvent failed (err=%lu)", ::GetLastError());
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    for (const std::wstring& root : roots) {
        if (root.empty()) continue;
        const HANDLE directory = ::CreateFileW(
            root.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE) {
            DF_WARN("shell", "launchpad watcher: cannot watch %s (err=%lu)",
                    core::WideToUtf8(root).c_str(), ::GetLastError());
            continue;
        }
        // Heap-allocated before arming, and never moved afterwards: the kernel
        // holds the address of `overlapped` for the duration of the read, so the
        // Watch has to sit at a stable address from the moment ReadDirectoryChangesW
        // is called. Building it on the stack and then moving it into the vector
        // was the bug that made the first version never fire.
        auto watch = std::make_unique<Impl::Watch>();
        watch->directory = directory;
        watch->overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (watch->overlapped.hEvent == nullptr) {
            ::CloseHandle(directory);
            continue;
        }
        // The initial async read arms the watch. Doing it here (rather than from
        // the thread) means a root that fails to arm is reported before Start
        // returns, instead of silently never firing.
        watch->buffer.resize(64 * 1024);
        if (!::ReadDirectoryChangesW(directory, watch->buffer.data(),
                                     static_cast<DWORD>(watch->buffer.size()), TRUE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE,
                                     nullptr, &watch->overlapped, nullptr)) {
            DF_WARN("shell", "launchpad watcher: ReadDirectoryChangesW failed for %s (err=%lu)",
                    core::WideToUtf8(root).c_str(), ::GetLastError());
            ::CloseHandle(directory);
            ::CloseHandle(watch->overlapped.hEvent);
            continue;
        }
        impl_->watches.push_back(std::move(watch));
    }

    if (impl_->watches.empty()) {
        DF_WARN("shell", "launchpad watcher: no directory could be watched");
        impl_->Stop();
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->thread = ::CreateThread(nullptr, 0, &Impl::ThreadMain, impl_, 0, nullptr);
    if (impl_->thread == nullptr) {
        DF_WARN("shell", "launchpad watcher: CreateThread failed (err=%lu)", ::GetLastError());
        impl_->Stop();
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    DF_INFO("shell", "launchpad watcher watching %zu directories", impl_->watches.size());
    return true;
}

bool LaunchpadWatcher::ConsumeChanged(double now_ms) {
    if (impl_ == nullptr) return false;
    if (impl_->changed.exchange(false, std::memory_order_relaxed)) {
        impl_->gate.NoteChange(now_ms);
    }
    return impl_->gate.Consume(now_ms);
}

void LaunchpadWatcher::Stop() {
    if (impl_ == nullptr) return;
    impl_->Stop();
    delete impl_;
    impl_ = nullptr;
}

int LaunchpadWatcher::WatchedCount() const {
    return impl_ == nullptr ? 0 : static_cast<int>(impl_->watches.size());
}

// ------------------------------------------------------------------ LaunchpadIndex

std::vector<std::wstring> DefaultWatchRoots(const core::LaunchpadConfig& config) {
    std::vector<std::wstring> roots;

    // FOLDERID_CommonPrograms is the machine-wide Start menu, FOLDERID_Programs the
    // per-user one. Both feed the shell AppsFolder, so a shortcut dropped in either
    // changes what the index would produce.
    for (const KNOWNFOLDERID* id : {&FOLDERID_CommonPrograms, &FOLDERID_Programs}) {
        PWSTR path = nullptr;
        if (SUCCEEDED(::SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &path)) &&
            path != nullptr) {
            roots.emplace_back(path);
        }
        if (path != nullptr) ::CoTaskMemFree(path);
    }

    for (const std::string& extra : config.extra_paths) {
        const std::wstring wide = core::Utf8ToWide(extra);
        if (wide.empty()) continue;
        // Only existing directories: ReadDirectoryChangesW on a missing path would
        // warn on every Launchpad open for a path the user simply unplugged.
        const DWORD attributes = ::GetFileAttributesW(wide.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        roots.push_back(wide);
    }

    std::sort(roots.begin(), roots.end());
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    return roots;
}

void LaunchpadIndex::Rebuild(const core::LaunchpadConfig& config) {
    const double started = NowMs();

    items_.clear();
    stats_ = LaunchpadIndexStats{};

    std::unordered_set<std::string> seen;
    seen.reserve(512);

    auto add_entry = [&](AppEntry&& entry, bool count_extra) {
        if (!LooksLikeApplication(entry)) {
            ++stats_.skipped;
            return;
        }
        std::string identity = IdentityOf(entry);
        if (identity.empty()) {
            ++stats_.skipped;
            return;
        }
        if (!seen.insert(identity).second) {
            ++stats_.duplicate_count;
            return;
        }
        LaunchpadItem item;
        item.app = std::move(entry);
        item.identity = std::move(identity);
        items_.push_back(std::move(item));
        if (count_extra) {
            ++stats_.extra_path_count;
        } else {
            ++stats_.apps_folder_accepted;
        }
    };

    // Source 1: AppsFolder. Same call the Dock makes, so a packaged app appears
    // here with the label the shell gives it rather than the AUMID.
    StartMenuScan scan = ScanAppsFolder();
    stats_.apps_folder_count = static_cast<int>(scan.entries.size());
    // Counted separately: these entries were dropped inside ScanAppsFolder and never
    // reached add_entry, so they are not part of `skipped`.
    stats_.apps_folder_skipped = scan.skipped;
    for (AppEntry& entry : scan.entries) add_entry(std::move(entry), false);

    // Source 2: user directories. Each root is walked with the same helper the
    // "all programs" view uses, so depth and extension rules cannot diverge.
    for (const std::string& path : config.extra_paths) {
        const std::wstring root = ToWidePath(path);
        if (root.empty()) continue;
        if (::GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
            DF_WARN("shell", "launchpad extra path missing: %s", path.c_str());
            ++stats_.path_failures;
            continue;
        }
        const std::vector<std::wstring> shortcuts = FindShortcuts(root, 4);
        if (shortcuts.empty()) {
            DF_DEBUG("shell", "launchpad extra path has no shortcuts: %s", path.c_str());
        }
        for (const std::wstring& shortcut : shortcuts) {
            AppEntry entry;
            const ResolveError error = ResolvePath(shortcut, entry);
            if (error != ResolveError::None || !entry.launchable()) {
                ++stats_.skipped;
                continue;
            }
            add_entry(std::move(entry), true);
        }
    }

    // Folders keep their own order (they are curated), so only the main grid is
    // sorted. Folders themselves are sorted by name in Folders().
    std::sort(items_.begin(), items_.end(), [](const LaunchpadItem& a, const LaunchpadItem& b) {
        return CompareNames(a.app.display_name, b.app.display_name) < 0;
    });

    stats_.scan_ms = NowMs() - started;
    DF_INFO("shell",
            "launchpad index: %zu items (apps=%d accepted=%d extra=%d dup=%d skipped=%d "
            "upstream=%d pathfail=%d) in %.1fms",
            items_.size(), stats_.apps_folder_count, stats_.apps_folder_accepted,
            stats_.extra_path_count, stats_.duplicate_count, stats_.skipped,
            stats_.apps_folder_skipped, stats_.path_failures, stats_.scan_ms);

    if (static_cast<int>(items_.size()) != stats_.ExpectedItemCount()) {
        DF_WARN("shell",
                "launchpad index census mismatch: items=%zu expected=%d (accepted=%d extra=%d "
                "dup=%d)",
                items_.size(), stats_.ExpectedItemCount(), stats_.apps_folder_accepted,
                stats_.extra_path_count, stats_.duplicate_count);
    }}

std::vector<LaunchpadFolderView> LaunchpadIndex::Folders(const core::LaunchpadConfig& config) const {
    std::vector<LaunchpadFolderView> views;
    views.reserve(config.folders.size());

    // Index by identity once instead of scanning items_ per folder entry: a user
    // with 64 folders of 20 items would otherwise do 1280 full passes.
    std::unordered_map<std::string, const LaunchpadItem*> by_identity;
    by_identity.reserve(items_.size() * 2);
    for (const LaunchpadItem& item : items_) by_identity.emplace(item.identity, &item);

    for (const core::LaunchpadFolder& folder : config.folders) {
        LaunchpadFolderView view;
        view.id = folder.id;
        view.name = folder.name.empty() ? folder.id : folder.name;
        for (const std::string& raw : folder.items) {
            const std::string identity = LowerAscii(raw);
            const auto it = by_identity.find(identity);
            if (it == by_identity.end()) {
                // Kept in the config, reported as missing: the app may be
                // uninstalled today and back tomorrow, and silently rewriting the
                // user's folder would be worse than a temporarily short grid.
                view.missing.push_back(identity);
                continue;
            }
            view.items.push_back(*it->second);
        }
        views.push_back(std::move(view));
    }

    std::sort(views.begin(), views.end(),
              [](const LaunchpadFolderView& a, const LaunchpadFolderView& b) {
                  const int by_name = CompareUtf8Names(a.name, b.name);
                  if (by_name != 0) return by_name < 0;
                  // Ties are possible with duplicate names; the id keeps the order
                  // stable so the grid does not reshuffle between frames.
                  return a.id < b.id;
              });
    return views;
}

std::vector<const LaunchpadItem*> LaunchpadIndex::Search(const std::wstring& query) const {
    std::vector<const LaunchpadItem*> matches;
    if (query.empty()) {
        matches.reserve(items_.size());
        for (const LaunchpadItem& item : items_) matches.push_back(&item);
        return matches;
    }

    const std::wstring lowered = LowerWide(query);
    matches.reserve(std::min<std::size_t>(items_.size(), 64));
    for (const LaunchpadItem& item : items_) {
        const std::wstring& name = item.app.display_name;
        if (name.size() < lowered.size()) continue;
        // Prefix, not substring: a Launchpad search that matched the middle of a
        // name would surface "Visual Studio Code" for "code", which is rarely what
        // the first keystrokes mean. A dedicated substring mode can come later.
        if (LowerWide(name.substr(0, lowered.size())) == lowered) matches.push_back(&item);
    }
    return matches;
}

}  // namespace dockforge::shell
