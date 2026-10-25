// T-066 unit tests: the folder drill-through browser's logic layer.
//
// The enumeration itself runs against a scratch directory created here, so the
// IShellFolder path (ADR-0023) is exercised for real; sorting, status mapping,
// placeholders and the navigation stack are pure. No window is created.
#include "catch_amalgamated.hpp"

#include "shell/folder_browser.h"

#include <windows.h>

#include <string>

namespace shell = dockforge::shell;

namespace {

// COM for SHParseDisplayName; a no-op when the suite already initialised it.
class ComScope {
  public:
    ComScope() { ok_ = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComScope() {
        if (ok_) ::CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

  private:
    bool ok_ = false;
};

// A scratch directory that cleans up after itself, including the subdirectories
// this test drills into (deeper than the flat delete in launchpad_test).
class TempTree {
  public:
    TempTree() {
        wchar_t base[MAX_PATH]{};
        if (::GetTempPathW(MAX_PATH, base) == 0) return;
        wchar_t unique[MAX_PATH]{};
        if (::GetTempFileNameW(base, L"dfb", 0, unique) == 0) return;
        ::DeleteFileW(unique);
        if (!::CreateDirectoryW(unique, nullptr)) return;
        path_ = unique;
    }
    ~TempTree() {
        if (path_.empty()) return;
        RemoveTree(path_);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const std::wstring& Path() const { return path_; }
    bool Valid() const { return !path_.empty(); }

    std::wstring Sub(const std::wstring& name) const {
        const std::wstring full = path_ + L"\\" + name;
        ::CreateDirectoryW(full.c_str(), nullptr);
        return full;
    }
    std::wstring File(const std::wstring& name) const {
        const std::wstring full = path_ + L"\\" + name;
        HANDLE h = ::CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        return full;
    }

  private:
    static void RemoveTree(const std::wstring& dir) {
        const std::wstring pattern = dir + L"\\*";
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            ::RemoveDirectoryW(dir.c_str());
            return;
        }
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                RemoveTree(full);
            } else {
                ::DeleteFileW(full.c_str());
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
        ::RemoveDirectoryW(dir.c_str());
    }

    std::wstring path_;
};

bool HasEntry(const shell::FolderListing& listing, const std::wstring& name, bool folder) {
    for (const shell::FolderEntry& entry : listing.entries) {
        if (entry.display_name == name && entry.is_folder == folder) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("folder status mapping", "[T066]") {
    using shell::FolderStatus;
    SECTION("success stays ok") {
        CHECK(shell::FolderStatusFromHresult(S_OK) == FolderStatus::Ok);
    }
    SECTION("missing path family") {
        CHECK(shell::FolderStatusFromHresult(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) ==
              FolderStatus::NotFound);
        CHECK(shell::FolderStatusFromHresult(HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) ==
              FolderStatus::NotFound);
    }
    SECTION("denied") {
        CHECK(shell::FolderStatusFromHresult(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) ==
              FolderStatus::AccessDenied);
    }
    SECTION("everything else is failed, not guessed") {
        CHECK(shell::FolderStatusFromHresult(E_FAIL) == FolderStatus::Failed);
        CHECK(shell::FolderStatusFromHresult(E_INVALIDARG) == FolderStatus::Failed);
    }
}

TEST_CASE("placeholder text exists for every status", "[T066]") {
    using shell::FolderStatus;
    // Every status must render something; the failure ones must not render the
    // same text as "empty", or a denied folder would look merely empty.
    const std::wstring ok_text = shell::PlaceholderFor(FolderStatus::Ok);
    const std::wstring not_found = shell::PlaceholderFor(FolderStatus::NotFound);
    const std::wstring denied = shell::PlaceholderFor(FolderStatus::AccessDenied);
    const std::wstring failed = shell::PlaceholderFor(FolderStatus::Failed);
    CHECK_FALSE(ok_text.empty());
    CHECK_FALSE(not_found.empty());
    CHECK_FALSE(denied.empty());
    CHECK_FALSE(failed.empty());
    CHECK(not_found != ok_text);
    CHECK(denied != ok_text);
    CHECK(failed != ok_text);
}

TEST_CASE("sort puts folders first and orders names case-insensitively", "[T066]") {
    std::vector<shell::FolderEntry> entries;
    auto add = [&entries](const wchar_t* name, bool folder, const wchar_t* path) {
        shell::FolderEntry e;
        e.display_name = name;
        e.full_path = path;
        e.is_folder = folder;
        entries.push_back(std::move(e));
    };
    add(L"zeta.txt", false, L"C:\\zeta.txt");
    add(L"beta", true, L"C:\\beta");
    add(L"Apple.txt", false, L"C:\\Apple.txt");
    add(L"alpha", true, L"C:\\alpha");
    add(L"apple.txt", false, L"C:\\apple_copy.txt");  // same folded name as Apple.txt

    shell::SortFolderEntries(entries);

    REQUIRE(entries.size() == 5);
    // Folders first, their own names folded-ascending.
    CHECK(entries[0].display_name == L"alpha");
    CHECK(entries[0].is_folder);
    CHECK(entries[1].display_name == L"beta");
    CHECK(entries[1].is_folder);
    // Then files; "Apple.txt" before "apple_copy.txt" would ALSO be true from a
    // case-sensitive sort, so the tie-break is pinned by the paths instead.
    CHECK(entries[2].display_name == L"Apple.txt");
    CHECK(entries[3].display_name == L"apple.txt");
    CHECK(entries[2].full_path == L"C:\\Apple.txt");
    CHECK(entries[3].full_path == L"C:\\apple_copy.txt");
    CHECK(entries[4].display_name == L"zeta.txt");
}

TEST_CASE("nav stack unwinds to the pinned root and stops there", "[T066]") {
    shell::FolderNavStack nav(L"C:\\root");
    CHECK(nav.AtRoot());
    CHECK(nav.Current() == L"C:\\root");
    CHECK(nav.Depth() == 1);

    nav.Push(L"C:\\root\\sub");
    CHECK_FALSE(nav.AtRoot());
    CHECK(nav.Current() == L"C:\\root\\sub");
    CHECK(nav.Depth() == 2);

    CHECK(nav.Pop());
    CHECK(nav.AtRoot());
    CHECK(nav.Current() == L"C:\\root");

    // Popping the root must fail - the panel closes on that instead of
    // navigating below the folder the user pinned to the Dock.
    CHECK_FALSE(nav.Pop());
    CHECK(nav.Current() == L"C:\\root");

    // A drill into nothing is not a navigation.
    nav.Push(L"");
    CHECK(nav.Depth() == 1);
    CHECK(nav.Root() == L"C:\\root");
}

TEST_CASE("ListFolder enumerates a real scratch directory", "[T066]") {
    ComScope com;
    TempTree tree;
    if (!tree.Valid()) {
        WARN("temp dir unavailable; skipping live enumeration");
        return;
    }
    tree.Sub(L"zz_subdir");
    tree.File(L"hello.txt");
    tree.File(L" readme .txt");  // display names are taken verbatim

    shell::FolderListing listing = shell::ListFolder(tree.Path());
    REQUIRE(listing.status == shell::FolderStatus::Ok);
    REQUIRE(listing.entries.size() == 3);
    // Folder first after the built-in sort.
    CHECK(listing.entries[0].display_name == L"zz_subdir");
    CHECK(listing.entries[0].is_folder);
    CHECK(HasEntry(listing, L"hello.txt", false));
    CHECK(HasEntry(listing, L" readme .txt", false));
    // Paths are full paths under the scratch dir.
    for (const shell::FolderEntry& entry : listing.entries) {
        CHECK(entry.full_path.rfind(tree.Path(), 0) == 0);
        CHECK_FALSE(entry.display_name.empty());
    }
    // Enumeration is cheap enough for a click path; pin it so a regression to
    // something frame-path-sized cannot slip in silently.
    CHECK(listing.elapsed_ms < 1000.0);
}

TEST_CASE("ListFolder handles the failure shapes the panel must survive", "[T066]") {
    ComScope com;

    SECTION("empty folder is Ok with zero entries") {
        TempTree tree;
        if (!tree.Valid()) {
            WARN("temp dir unavailable; skipping");
            return;
        }
        const std::wstring empty_dir = tree.Sub(L"empty");
        shell::FolderListing listing = shell::ListFolder(empty_dir);
        CHECK(listing.status == shell::FolderStatus::Ok);
        CHECK(listing.entries.empty());
    }

    SECTION("missing folder is NotFound, not a crash") {
        shell::FolderListing listing =
            shell::ListFolder(L"C:\\__dockforge_no_such_dir_t066__");
        CHECK(listing.status == shell::FolderStatus::NotFound);
        CHECK(listing.entries.empty());
    }

    SECTION("trailing separators do not change the answer") {
        TempTree tree;
        if (!tree.Valid()) {
            WARN("temp dir unavailable; skipping");
            return;
        }
        tree.Sub(L"d");
        shell::FolderListing listing = shell::ListFolder(tree.Path() + L"\\d\\\\\\");
        CHECK(listing.status == shell::FolderStatus::Ok);
        // The entry path is canonicalised the same way, so icon cache keys and
        // drill targets stay stable across the two spellings.
        shell::FolderListing plain = shell::ListFolder(tree.Path() + L"\\d");
        REQUIRE(listing.entries.size() == plain.entries.size());
        if (listing.entries.size() == plain.entries.size() && !listing.entries.empty()) {
            CHECK(listing.entries[0].full_path == plain.entries[0].full_path);
        }
    }
}
