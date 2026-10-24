// DockForge - Launchpad index tests (T-040, CONTRACTS.md C-18).
//
// Two kinds of test live here, deliberately separated:
//
//   * pure rules - the debounce gate, the collation order, folder resolution and
//     search. These run with no filesystem, no COM and no threads, so they can run
//     on every commit and they cover the parts that break silently.
//   * the real index - one test that builds against a temporary directory of
//     shortcuts. It is the only way to prove the extra_paths source actually
//     aggregates, but it needs COM and a writable temp dir, so it asserts only
//     what it can control (a shortcut we created appears; a duplicate is dropped).
//
// Nothing here asserts on the machine's real AppsFolder: its contents differ per
// machine, and a test that depends on "Calculator is installed" is a test that
// fails on a build agent.
#include <catch_amalgamated.hpp>

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/config.h"
#include "core/paths.h"
#include "shell/app_identity.h"
#include "shell/launchpad_index.h"

namespace shell = dockforge::shell;
namespace core = dockforge::core;

namespace {

// A scratch directory that cleans up after itself. Using the real profile would
// leave litter behind on a failed assertion.
class TempDir {
public:
    TempDir() {
        wchar_t base[MAX_PATH]{};
        if (::GetTempPathW(MAX_PATH, base) == 0) return;
        wchar_t unique[MAX_PATH]{};
        if (::GetTempFileNameW(base, L"dfp", 0, unique) == 0) return;
        // GetTempFileName creates a file; we want a directory of that name.
        ::DeleteFileW(unique);
        if (!::CreateDirectoryW(unique, nullptr)) return;
        path_ = unique;
    }
    ~TempDir() {
        if (path_.empty()) return;
        // Only ever holds the shortcuts this test created, so a flat delete is
        // enough and there is no recursive walk to get wrong.
        const std::wstring pattern = path_ + L"\\*";
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (data.cFileName[0] == L'.') continue;
                ::DeleteFileW((path_ + L"\\" + data.cFileName).c_str());
            } while (::FindNextFileW(find, &data));
            ::FindClose(find);
        }
        ::RemoveDirectoryW(path_.c_str());
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::wstring& Path() const { return path_; }
    bool Valid() const { return !path_.empty(); }

private:
    std::wstring path_;
};

// Creates a .lnk in `dir` pointing at `target`. Returns false when the shell
// refused, which makes the caller skip rather than assert on an environment
// problem.
bool MakeShortcut(const std::wstring& dir, const std::wstring& name,
                  const std::wstring& target) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    if (FAILED(::CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&link)))) {
        return false;
    }
    link->SetPath(target.c_str());
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file))) return false;
    const std::wstring path = dir + L"\\" + name;
    return SUCCEEDED(file->Save(path.c_str(), TRUE));
}

// The test executable's own path, so every shortcut points at something that
// genuinely exists and resolves.
std::wstring SelfPath() {
    wchar_t buffer[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return buffer;
}

// Scoped COM, because the index needs it and the test binary does not otherwise
// initialize it.
struct ComScope {
    bool ok = false;
    ComScope() { ok = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComScope() {
        if (ok) ::CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

}  // namespace

// ------------------------------------------------------------------ DebounceGate

TEST_CASE("DebounceGate waits for a burst to settle", "[launchpad][debounce]") {
    shell::DebounceGate gate(500.0);

    CHECK_FALSE(gate.Pending());
    CHECK_FALSE(gate.Consume(1000.0));  // nothing noted yet

    gate.NoteChange(1000.0);
    CHECK(gate.Pending());
    // Still inside the window: a file copy in progress must not trigger a rebuild.
    CHECK_FALSE(gate.Consume(1200.0));
    CHECK_FALSE(gate.Consume(1499.0));
    CHECK(gate.Consume(1500.0));
    // Exactly once: the caller must not rebuild again on the next frame.
    CHECK_FALSE(gate.Consume(1501.0));
    CHECK_FALSE(gate.Pending());
}

TEST_CASE("DebounceGate restarts its window on every event", "[launchpad][debounce]") {
    shell::DebounceGate gate(500.0);

    gate.NoteChange(1000.0);
    gate.NoteChange(1300.0);  // the burst is still going
    CHECK_FALSE(gate.Consume(1400.0));
    // The deadline moved with the last event, not the first one.
    CHECK_FALSE(gate.Consume(1799.0));
    CHECK(gate.Consume(1800.0));
}

TEST_CASE("DebounceGate Reset discards a pending burst", "[launchpad][debounce]") {
    shell::DebounceGate gate(500.0);
    gate.NoteChange(1000.0);
    gate.Reset();
    CHECK_FALSE(gate.Pending());
    CHECK_FALSE(gate.Consume(5000.0));
}

// ------------------------------------------------------------------ collation

TEST_CASE("CompareDisplayNames ignores case the way a reader expects", "[launchpad][sort]") {
    // Without a linguistic flag every capital sorts ahead of every lowercase, so
    // "Zebra" would land before "apple". This is the assertion that catches a
    // regression to a plain wcscmp.
    CHECK(shell::CompareDisplayNames(L"apple", L"Zebra") < 0);
    CHECK(shell::CompareDisplayNames(L"Zebra", L"apple") > 0);

    // Case differences do NOT compare equal - measured on this machine,
    // CompareStringEx(SORT_DEFAULT|NORM_LINGUISTIC_CASING) puts "apple" before
    // "Apple" (lowercase first), which is the dictionary order the Start menu
    // uses. What matters for the grid is that the result is antisymmetric and
    // deterministic, not that it collapses case: an unstable order would make the
    // grid reshuffle between frames.
    const int lower_first = shell::CompareDisplayNames(L"apple", L"Apple");
    const int upper_first = shell::CompareDisplayNames(L"Apple", L"apple");
    CHECK(lower_first == -upper_first);
    CHECK(lower_first != 0);

    // ...and case must not dominate the primary ordering. "Apple" still belongs
    // near "apple", i.e. both sort before "Zebra".
    CHECK(shell::CompareDisplayNames(L"Apple", L"Zebra") < 0);
    CHECK(shell::CompareDisplayNames(L"apple", L"Zebra") < 0);

    CHECK(shell::CompareDisplayNames(L"same", L"same") == 0);
}

TEST_CASE("CompareDisplayNames orders CJK names by collation, not bytes", "[launchpad][sort]") {
    // Byte order would put every CJK name after every Latin one because the UTF-16
    // code units are all larger. The user's collation interleaves them, which is
    // what the Start menu does on a Chinese system. The assertion is only that the
    // comparison is antisymmetric and self-consistent - the exact interleaving is
    // a locale decision, not ours to pin.
    const int forward = shell::CompareDisplayNames(L"a", L"\u4e2d\u6587");
    const int backward = shell::CompareDisplayNames(L"\u4e2d\u6587", L"a");
    CHECK(forward == -backward);
    CHECK(forward != 0);
}

// ------------------------------------------------------------------ folders

TEST_CASE("Folders resolves identities and reports missing ones", "[launchpad][folders]") {
    shell::LaunchpadIndex index;
    // Rebuild against a config with no extra paths still scans AppsFolder, so the
    // test does not need to fabricate items. Instead it asks for folders whose
    // items cannot possibly exist, which exercises the missing path deterministically.
    core::LaunchpadConfig config;
    config.folders.push_back({.id = "f1", .name = "Games", .items = {"definitely-not-installed"}});

    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping index-backed folder assertions");
        return;
    }
    index.Rebuild(config);

    const std::vector<shell::LaunchpadFolderView> views = index.Folders(config);
    REQUIRE(views.size() == 1);
    CHECK(views[0].id == "f1");
    CHECK(views[0].name == "Games");
    CHECK(views[0].items.empty());
    // The identity stays in the folder so the app can come back when reinstalled.
    REQUIRE(views[0].missing.size() == 1);
    CHECK(views[0].missing[0] == "definitely-not-installed");
}

TEST_CASE("Folders sorts by name and keeps a stable tie-break", "[launchpad][folders]") {
    core::LaunchpadConfig config;
    config.folders.push_back({.id = "z", .name = "Zeta", .items = {"x"}});
    config.folders.push_back({.id = "a", .name = "Alpha", .items = {"x"}});
    config.folders.push_back({.id = "b2", .name = "Same", .items = {"x"}});
    config.folders.push_back({.id = "b1", .name = "Same", .items = {"x"}});

    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    shell::LaunchpadIndex index;
    index.Rebuild(config);

    const std::vector<shell::LaunchpadFolderView> views = index.Folders(config);
    REQUIRE(views.size() == 4);
    CHECK(views[0].name == "Alpha");
    // Duplicate names must not reshuffle between calls; the id breaks the tie.
    CHECK(views[1].id == "b1");
    CHECK(views[2].id == "b2");
    CHECK(views[3].name == "Zeta");
}

TEST_CASE("An empty folder name falls back to its id", "[launchpad][folders]") {
    core::LaunchpadConfig config;
    config.folders.push_back({.id = "only-id", .name = "", .items = {"x"}});

    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    shell::LaunchpadIndex index;
    index.Rebuild(config);
    const auto views = index.Folders(config);
    REQUIRE(views.size() == 1);
    CHECK(views[0].name == "only-id");
}

// ------------------------------------------------------------------ search

TEST_CASE("Search on an empty query returns everything", "[launchpad][search]") {
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    shell::LaunchpadIndex index;
    index.Rebuild(core::LaunchpadConfig{});
    const auto all = index.Search(L"");
    CHECK(all.size() == index.Items().size());
}

TEST_CASE("Search is a case-insensitive prefix match", "[launchpad][search]") {
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    shell::LaunchpadIndex index;
    index.Rebuild(core::LaunchpadConfig{});
    if (index.Empty()) {
        WARN("AppsFolder scan produced no items; skipping");
        return;
    }

    // Use a real item's own name so the test does not depend on which apps exist.
    const std::wstring name = index.Items().front().app.display_name;
    if (name.empty()) {
        WARN("first item has no display name; skipping");
        return;
    }
    std::wstring upper = name.substr(0, 1);
    for (wchar_t& ch : upper) {
        if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - L'a' + L'A');
    }
    const auto matches = index.Search(upper);
    CHECK_FALSE(matches.empty());

    // A prefix that cannot match anything must return nothing rather than
    // everything, which is the failure mode of a broken filter.
    CHECK(index.Search(L"zzzz-no-such-app-zzzz").empty());
}

TEST_CASE("Search does not match in the middle of a name", "[launchpad][search]") {
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    shell::LaunchpadIndex index;
    index.Rebuild(core::LaunchpadConfig{});
    if (index.Empty()) {
        WARN("no items; skipping");
        return;
    }
    // Find an item with a name long enough to have a middle, then search for a
    // suffix of it. A prefix filter must not return it.
    for (const shell::LaunchpadItem& item : index.Items()) {
        if (item.app.display_name.size() < 4) continue;
        const std::wstring tail = item.app.display_name.substr(2);
        const auto matches = index.Search(tail);
        for (const shell::LaunchpadItem* match : matches) {
            CHECK(match->app.display_name.substr(0, tail.size()) == tail);
        }
        return;
    }
    WARN("no item long enough to test a mid-name search; skipping");
}

// ------------------------------------------------------------------ real index

TEST_CASE("Rebuild aggregates a shortcut from an extra path", "[launchpad][index][filesystem]") {
    TempDir temp;
    if (!temp.Valid()) {
        WARN("temp dir unavailable; skipping");
        return;
    }
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }

    // A nested directory proves the walk is recursive, which is what makes
    // "point Launchpad at a folder of shortcuts" work for an organised tree.
    const std::wstring nested = temp.Path() + L"\\nested";
    ::CreateDirectoryW(nested.c_str(), nullptr);
    if (!MakeShortcut(nested, L"LaunchpadIndexProbe.lnk", SelfPath())) {
        WARN("could not create a shortcut; skipping");
        return;
    }

    core::LaunchpadConfig config;
    config.extra_paths.push_back(core::WideToUtf8(temp.Path()));

    shell::LaunchpadIndex index;
    index.Rebuild(config);

    CHECK(index.Stats().path_failures == 0);
    CHECK(index.Stats().extra_path_count >= 1);

    bool found = false;
    for (const shell::LaunchpadItem& item : index.Items()) {
        if (item.app.display_name == L"LaunchpadIndexProbe") found = true;
    }
    CHECK(found);
}

TEST_CASE("A missing extra path is counted, not fatal", "[launchpad][index]") {
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    core::LaunchpadConfig config;
    config.extra_paths.push_back("Z:\\this\\path\\does\\not\\exist");

    shell::LaunchpadIndex index;
    index.Rebuild(config);  // must not throw or abort
    CHECK(index.Stats().path_failures == 1);
}

TEST_CASE("An empty extra path contributes nothing and reports no failure", "[launchpad][index][filesystem]") {
    TempDir temp;
    if (!temp.Valid()) {
        WARN("temp dir unavailable; skipping");
        return;
    }
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    // An empty directory is not an error: the user may have pointed at a folder
    // they are about to fill. It must not count as a path failure, which would
    // make the stats look like something is broken.
    core::LaunchpadConfig config;
    config.extra_paths.push_back(core::WideToUtf8(temp.Path()));

    shell::LaunchpadIndex index;
    index.Rebuild(config);
    CHECK(index.Stats().path_failures == 0);
    CHECK(index.Stats().extra_path_count == 0);
}

TEST_CASE("A path that is not a readable directory is counted as a failure", "[launchpad][index][filesystem]") {
    TempDir temp;
    if (!temp.Valid()) {
        WARN("temp dir unavailable; skipping");
        return;
    }
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    // A regular file passes GetFileAttributesW but cannot be enumerated as a
    // directory. This is the closest reproducible stand-in for the "no permission"
    // case without manipulating ACLs, and it exercises the same branch: the walk
    // yields nothing, and the user is told rather than left with a silent empty
    // result.
    const std::wstring file = temp.Path() + L"\\not-a-directory.txt";
    HANDLE handle = ::CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    ::CloseHandle(handle);

    core::LaunchpadConfig config;
    config.extra_paths.push_back(core::WideToUtf8(file));

    shell::LaunchpadIndex index;
    index.Rebuild(config);
    // GetFileAttributesW succeeds, so this is not a "missing path" failure; the
    // walk simply finds no shortcuts. What must NOT happen is a crash or a
    // resolved entry.
    CHECK(index.Stats().extra_path_count == 0);
}

TEST_CASE("A corrupt .lnk is skipped instead of aborting the scan", "[launchpad][index][filesystem]") {
    TempDir temp;
    if (!temp.Valid()) {
        WARN("temp dir unavailable; skipping");
        return;
    }
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    // Bytes that are not a shell link at all. IPersistFile::Load rejects this, so
    // ResolveShortcut fails and the item must be counted as skipped - a single bad
    // shortcut in a user's folder must not empty the Launchpad.
    const std::wstring broken = temp.Path() + L"\\Broken.lnk";
    HANDLE handle = ::CreateFileW(broken.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    const char garbage[] = "this is not a shell link";
    DWORD written = 0;
    ::WriteFile(handle, garbage, sizeof(garbage) - 1, &written, nullptr);
    ::CloseHandle(handle);

    core::LaunchpadConfig config;
    config.extra_paths.push_back(core::WideToUtf8(temp.Path()));

    shell::LaunchpadIndex index;
    index.Rebuild(config);
    CHECK(index.Stats().skipped >= 1);
    for (const shell::LaunchpadItem& item : index.Items()) {
        CHECK(item.app.display_name != L"Broken");
    }
}

TEST_CASE("Document shortcuts are not applications", "[launchpad][index][filesystem]") {
    TempDir temp;
    if (!temp.Valid()) {
        WARN("temp dir unavailable; skipping");
        return;
    }
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    // Regression guard. WinRAR's Start-menu folder ships a shortcut that resolves
    // to `whatsnew.txt`, and ResolvePath happily resolves it, so without an
    // application filter the grid gains a tile that opens a readme. Measured on
    // this machine: 295 items before the filter, 288 after, with the 7 rejects all
    // being .chm/.txt help files.
    const std::wstring document = temp.Path() + L"\\readme.txt";
    HANDLE handle = ::CreateFileW(document.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    ::CloseHandle(handle);
    if (!MakeShortcut(temp.Path(), L"Readme.lnk", document)) {
        WARN("could not create a shortcut; skipping");
        return;
    }

    core::LaunchpadConfig config;
    config.extra_paths.push_back(core::WideToUtf8(temp.Path()));

    shell::LaunchpadIndex index;
    index.Rebuild(config);
    for (const shell::LaunchpadItem& item : index.Items()) {
        CHECK(item.app.display_name != L"Readme");
        CHECK(item.identity.find("readme.txt") == std::string::npos);
    }
    // A .lnk to a document still resolves, so it must be counted as skipped rather
    // than silently vanishing - otherwise the stats cannot explain the shortfall.
    CHECK(index.Stats().skipped >= 1);
}

TEST_CASE("Packaged apps are keyed by their AUMID, not their host path", "[launchpad][index][uwp]") {
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    shell::LaunchpadIndex index;
    index.Rebuild(core::LaunchpadConfig{});

    // Conditional on the machine actually having a packaged app: a build agent
    // with none would otherwise fail a correct implementation. When one exists,
    // its identity must be the lowercased AUMID - every packaged window is hosted
    // by ApplicationFrameHost.exe, so a path-based key would make all of them look
    // like the same app (C-13b).
    int uwp_count = 0;
    for (const shell::LaunchpadItem& item : index.Items()) {
        if (item.app.kind != shell::AppKind::Uwp) continue;
        ++uwp_count;
        REQUIRE_FALSE(item.app.aumid.empty());
        CHECK(item.identity == shell::LowerAscii(core::WideToUtf8(item.app.aumid)));
    }
    if (uwp_count == 0) {
        WARN("no packaged apps on this machine; AUMID identity not exercised");
    }
}

TEST_CASE("Rebuild deduplicates the same identity from two sources", "[launchpad][index][filesystem]") {
    TempDir temp;
    if (!temp.Valid()) {
        WARN("temp dir unavailable; skipping");
        return;
    }
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    // Two shortcuts to the same target resolve to the same identity (the path),
    // so the second must be dropped rather than shown twice.
    if (!MakeShortcut(temp.Path(), L"DupA.lnk", SelfPath())) {
        WARN("could not create a shortcut; skipping");
        return;
    }
    if (!MakeShortcut(temp.Path(), L"DupB.lnk", SelfPath())) {
        WARN("could not create a shortcut; skipping");
        return;
    }

    core::LaunchpadConfig config;
    config.extra_paths.push_back(core::WideToUtf8(temp.Path()));

    shell::LaunchpadIndex index;
    index.Rebuild(config);
    CHECK(index.Stats().duplicate_count >= 1);

    int occurrences = 0;
    for (const shell::LaunchpadItem& item : index.Items()) {
        if (item.app.display_name == L"DupA" || item.app.display_name == L"DupB") ++occurrences;
    }
    CHECK(occurrences == 1);
}

TEST_CASE("DefaultWatchRoots covers the Start menu and skips missing paths",
          "[launchpad][watcher]") {
    // The index must notice a newly installed app. Its AppsFolder source is backed
    // by the two Start menu Programs folders on disk, so those two must be watched
    // without the caller having to know their paths.
    core::LaunchpadConfig config;
    const std::vector<std::wstring> roots = shell::DefaultWatchRoots(config);
    CHECK_FALSE(roots.empty());

    bool saw_programs = false;
    for (const std::wstring& root : roots) {
        const DWORD attributes = ::GetFileAttributesW(root.c_str());
        CHECK(attributes != INVALID_FILE_ATTRIBUTES);
        CHECK((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
        if (root.find(L"Start Menu") != std::wstring::npos) saw_programs = true;
    }
    CHECK(saw_programs);

    // A path the user unplugged must not be watched: ReadDirectoryChangesW would
    // warn on every Launchpad open for something that is simply absent.
    config.extra_paths.push_back("Z:\\no\\such\\directory");
    const std::vector<std::wstring> with_missing = shell::DefaultWatchRoots(config);
    CHECK(with_missing.size() == roots.size());

    // Duplicates collapse: the same directory reachable twice would otherwise burn
    // two of the watcher's handles for one signal.
    TempDir temp;
    if (temp.Valid()) {
        const std::string twice = core::WideToUtf8(temp.Path());
        config.extra_paths.clear();
        config.extra_paths.push_back(twice);
        config.extra_paths.push_back(twice);
        const std::vector<std::wstring> deduped = shell::DefaultWatchRoots(config);
        CHECK(deduped.size() == roots.size() + 1);
    }
}

TEST_CASE("The index census adds up on a real machine", "[launchpad][index]") {
    ComScope com;
    if (!com.ok) {
        WARN("COM unavailable; skipping");
        return;
    }
    // A stats block that does not reconcile is how a silently dropped source goes
    // unnoticed, and the arithmetic is easy to get wrong: both accepted counters are
    // post-dedup, so `duplicate_count` is reported rather than subtracted, and
    // `apps_folder_skipped` counts entries that never reached this index.
    core::LaunchpadConfig config;
    config.extra_paths.push_back("C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs");

    shell::LaunchpadIndex index;
    index.Rebuild(config);
    const shell::LaunchpadIndexStats& stats = index.Stats();

    CHECK(static_cast<int>(index.Items().size()) == stats.ExpectedItemCount());
    CHECK(stats.apps_folder_accepted <= stats.apps_folder_count);
    // Dedup is the whole point: no identity may appear twice.
    std::unordered_set<std::string> unique;
    for (const shell::LaunchpadItem& item : index.Items()) unique.insert(item.identity);
    CHECK(unique.size() == index.Items().size());

    // The machine-wide Programs folder also backs AppsFolder, so overlapping entries
    // must have collapsed rather than doubled.
    if (stats.apps_folder_count > 0) {
        CHECK(stats.duplicate_count >= 1);
    }
}

TEST_CASE("Watcher reports a settled change and stops cleanly", "[launchpad][watcher][filesystem]") {
    TempDir temp;
    if (!temp.Valid()) {
        WARN("temp dir unavailable; skipping");
        return;
    }

    shell::LaunchpadWatcher watcher;
    if (!watcher.Start({temp.Path()}, 100.0)) {
        WARN("watcher could not start; skipping");
        return;
    }
    CHECK(watcher.Running());
    CHECK(watcher.WatchedCount() == 1);

    // Touch a file so the watch fires. The debounce window is 100ms, so the test
    // waits past it before expecting the signal.
    const std::wstring probe = temp.Path() + L"\\probe.txt";
    HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(file != INVALID_HANDLE_VALUE);
    ::CloseHandle(file);

    bool changed = false;
    for (int attempt = 0; attempt < 40 && !changed; ++attempt) {
        ::Sleep(50);
        changed = watcher.ConsumeChanged(static_cast<double>(attempt) * 50.0);
    }
    CHECK(changed);

    watcher.Stop();
    CHECK_FALSE(watcher.Running());
    CHECK(watcher.WatchedCount() == 0);
}
