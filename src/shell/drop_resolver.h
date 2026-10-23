// DockForge - turn a path that arrived via OLE drop into an AppEntry.
// Contract: CONTRACTS.md C-16.
//
// A drop is untrusted input: the pointer may carry a folder, a shortcut, a .exe, a
// document, or something that only looks like a path. Classification happens here,
// before any COM work, so an unacceptable drop costs one GetFileAttributesW instead
// of a shell round trip - and so the "can this be accepted" question the drag
// feedback needs is answerable without touching the filesystem twice.
#pragma once

#include <string>
#include <vector>

#include "shell/app_entry.h"
#include "shell/app_resolver.h"

namespace dockforge::shell {

// What a dropped path is, as far as the dock cares.
enum class DropKind {
    None,         // empty string
    Shortcut,     // .lnk
    Executable,   // .exe / .com / .bat / .cmd
    Folder,       // a directory
    Unsupported,  // everything else, including documents
};

const char* ToString(DropKind kind);

// True for the kinds the dock will accept onto its tile row.
bool IsDockable(DropKind kind);

// Classifies one path. `attributes` is the value from GetFileAttributesW; passing
// INVALID_FILE_ATTRIBUTES means the path does not exist and yields None.
DropKind ClassifyDroppedPath(const std::wstring& path, unsigned long attributes);

// Convenience wrapper that queries the attributes itself.
DropKind ClassifyDroppedPath(const std::wstring& path);

// Classifies a whole payload. The dock accepts a multi-file drop only when every
// path is dockable: accepting a mixed payload would silently discard the part it
// cannot use, and the user would have no way to know which part landed.
DropKind ClassifyDroppedPaths(const std::vector<std::wstring>& paths);

// Resolves a dropped path into an entry. Folders become AppKind::Folder and are
// launched through ShellExecuteEx; everything else goes through ResolvePath, so a
// .lnk is expanded exactly the way a Start-menu shortcut is.
ResolveError ResolveDroppedPath(const std::wstring& path, AppEntry& out);

}  // namespace dockforge::shell
