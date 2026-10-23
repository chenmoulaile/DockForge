// DockForge - drop classification and OLE drop-target helpers (T-014, C-16).
//
// Only the pure parts are covered here: classification, insertion-slot arithmetic and
// the OLE session's idempotency. The drag-and-drop handshake itself cannot be tested
// without a second process actually dragging, so it is covered by evidence/T-014
// instead - a unit test that faked the handshake would assert our own stub.
#include "dock/drop_target.h"
#include "shell/drop_resolver.h"

#include <catch_amalgamated.hpp>

#include <windows.h>

#include <string>
#include <vector>

namespace dock = dockforge::dock;
namespace shell = dockforge::shell;

namespace {

std::wstring TempDir() {
    wchar_t buffer[MAX_PATH]{};
    const DWORD len = GetTempPathW(MAX_PATH, buffer);
    std::wstring dir(buffer, len);
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    return dir;
}

// Creates an empty file, returning its path. Removed by the caller.
std::wstring MakeFile(const std::wstring& name) {
    const std::wstring path = TempDir() + L"\\dockforge-drop-" +
                              std::to_wstring(GetCurrentProcessId()) + L"-" + name;
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    return path;
}

std::wstring MakeDir() {
    const std::wstring path = TempDir() + L"\\dockforge-drop-dir-" +
                              std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

struct Cleanup {
    std::vector<std::wstring> files;
    std::vector<std::wstring> dirs;
    ~Cleanup() {
        for (const auto& f : files) DeleteFileW(f.c_str());
        for (const auto& d : dirs) RemoveDirectoryW(d.c_str());
    }
};

}  // namespace

TEST_CASE("drop: ToString names every kind", "[dock][drop]") {
    CHECK(std::string(shell::ToString(shell::DropKind::None)) == "none");
    CHECK(std::string(shell::ToString(shell::DropKind::Shortcut)) == "shortcut");
    CHECK(std::string(shell::ToString(shell::DropKind::Executable)) == "executable");
    CHECK(std::string(shell::ToString(shell::DropKind::Folder)) == "folder");
    CHECK(std::string(shell::ToString(shell::DropKind::Unsupported)) == "unsupported");
}

TEST_CASE("drop: only shortcuts, executables and folders are dockable", "[dock][drop]") {
    CHECK(shell::IsDockable(shell::DropKind::Shortcut));
    CHECK(shell::IsDockable(shell::DropKind::Executable));
    CHECK(shell::IsDockable(shell::DropKind::Folder));
    // A document is not a launcher target: pinning one would produce a tile that opens
    // an editor, which is a different action from the one the tile appears to offer.
    CHECK_FALSE(shell::IsDockable(shell::DropKind::Unsupported));
    CHECK_FALSE(shell::IsDockable(shell::DropKind::None));
}

TEST_CASE("drop: classification is driven by attributes and extension", "[dock][drop]") {
    // A directory wins over any extension it might appear to have: "notes.txt" as a
    // folder is still a folder.
    CHECK(shell::ClassifyDroppedPath(L"C:\\anything\\notes.txt", FILE_ATTRIBUTE_DIRECTORY) ==
          shell::DropKind::Folder);
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\b.lnk", FILE_ATTRIBUTE_NORMAL) ==
          shell::DropKind::Shortcut);
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\b.EXE", FILE_ATTRIBUTE_NORMAL) ==
          shell::DropKind::Executable);
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\b.cmd", FILE_ATTRIBUTE_NORMAL) ==
          shell::DropKind::Executable);
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\b.bat", FILE_ATTRIBUTE_NORMAL) ==
          shell::DropKind::Executable);
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\b.com", FILE_ATTRIBUTE_NORMAL) ==
          shell::DropKind::Executable);
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\b.txt", FILE_ATTRIBUTE_NORMAL) ==
          shell::DropKind::Unsupported);
    // Extensionless files are not guessed at: an .exe that lost its extension is
    // indistinguishable from a binary document, and guessing wrong means a tile that
    // fails when clicked.
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\noext", FILE_ATTRIBUTE_NORMAL) ==
          shell::DropKind::Unsupported);
    CHECK(shell::ClassifyDroppedPath(L"", FILE_ATTRIBUTE_NORMAL) == shell::DropKind::None);
    CHECK(shell::ClassifyDroppedPath(L"C:\\a\\b.exe", INVALID_FILE_ATTRIBUTES) ==
          shell::DropKind::None);
}

TEST_CASE("drop: the single-argument form queries the filesystem", "[dock][drop]") {
    Cleanup cleanup;
    const std::wstring dir = MakeDir();
    cleanup.dirs.push_back(dir);
    const std::wstring exe = MakeFile(L"fake.exe");
    cleanup.files.push_back(exe);
    const std::wstring doc = MakeFile(L"fake.txt");
    cleanup.files.push_back(doc);

    CHECK(shell::ClassifyDroppedPath(dir) == shell::DropKind::Folder);
    CHECK(shell::ClassifyDroppedPath(exe) == shell::DropKind::Executable);
    CHECK(shell::ClassifyDroppedPath(doc) == shell::DropKind::Unsupported);
    CHECK(shell::ClassifyDroppedPath(TempDir() + L"\\dockforge-does-not-exist-xyz.exe") ==
          shell::DropKind::None);
}

TEST_CASE("drop: a mixed payload is rejected as a whole", "[dock][drop]") {
    Cleanup cleanup;
    const std::wstring exe = MakeFile(L"one.exe");
    cleanup.files.push_back(exe);
    const std::wstring doc = MakeFile(L"two.txt");
    cleanup.files.push_back(doc);

    CHECK(shell::ClassifyDroppedPaths({}) == shell::DropKind::None);
    CHECK(shell::ClassifyDroppedPaths({exe}) == shell::DropKind::Executable);
    // Accepting half a payload would land an unpredictable subset and give the user no
    // way to tell which half worked.
    CHECK(shell::ClassifyDroppedPaths({exe, doc}) == shell::DropKind::Unsupported);
    CHECK(shell::ClassifyDroppedPaths({doc, exe}) == shell::DropKind::Unsupported);
    // A missing path also poisons the batch: it is not "the file I dragged".
    CHECK(shell::ClassifyDroppedPaths({exe, TempDir() + L"\\dockforge-nope.exe"}) ==
          shell::DropKind::None);
}

TEST_CASE("drop: a folder resolves to a launchable folder entry", "[dock][drop]") {
    Cleanup cleanup;
    const std::wstring dir = MakeDir();
    cleanup.dirs.push_back(dir);

    shell::AppEntry entry;
    REQUIRE(shell::ResolveDroppedPath(dir, entry) == shell::ResolveError::None);
    CHECK(entry.kind == shell::AppKind::Folder);
    CHECK(entry.target_path == dir);
    // The folder is its own icon source: the shell returns the standard folder icon,
    // and pointing at a .lnk would open a second shell round trip for nothing.
    CHECK(entry.icon_source == dir);
    CHECK_FALSE(entry.display_name.empty());
    CHECK(entry.display_name.back() != L'\\');
    CHECK(entry.launchable());
}

TEST_CASE("drop: resolving rejects paths the dock would never accept", "[dock][drop]") {
    shell::AppEntry entry;
    CHECK(shell::ResolveDroppedPath(L"", entry) == shell::ResolveError::EmptyPath);
    CHECK(shell::ResolveDroppedPath(TempDir() + L"\\dockforge-missing-dir-xyz", entry) ==
          shell::ResolveError::NotFound);
}

TEST_CASE("drop: insertion slot counts the centres the pointer has passed", "[dock][drop]") {
    const std::vector<float> centres{24.0f, 80.0f, 136.0f, 192.0f, 248.0f};
    CHECK(dock::DockDropTarget::InsertIndexFor(0.0f, centres) == 0);
    CHECK(dock::DockDropTarget::InsertIndexFor(23.0f, centres) == 0);
    // On a centre is on that tile, not before it - the same rule as the internal
    // reorder (C-15d). Off-by-one here puts the dropped tile one slot to the left of
    // where the caret was drawn.
    CHECK(dock::DockDropTarget::InsertIndexFor(24.0f, centres) == 1);
    CHECK(dock::DockDropTarget::InsertIndexFor(100.0f, centres) == 2);
    CHECK(dock::DockDropTarget::InsertIndexFor(248.0f, centres) == 5);
    // Past the last centre is "append", which is index == size, not size - 1.
    CHECK(dock::DockDropTarget::InsertIndexFor(1000.0f, centres) == 5);
    CHECK(dock::DockDropTarget::InsertIndexFor(0.0f, {}) == 0);
}

TEST_CASE("drop: the OLE session is idempotent", "[dock][drop]") {
    dock::OleSession first;
    // The test process already ran CoInitializeEx for the shell tests, so OLE may
    // report S_FALSE (already initialised) - but it must not fail, or RegisterDragDrop
    // would refuse to register.
    CHECK(first.ok());
    dock::OleSession second;
    CHECK(second.ok());
    CHECK(second.result() == S_FALSE);
}
