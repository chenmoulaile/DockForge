// DockForge - resolve a shell item (.lnk / .exe / UWP) into an AppEntry.
// Contract: CONTRACTS.md C-08.
//
// Every function that can fail returns a ResolveError instead of throwing, and
// never leaves the output entry half-populated. The Dock resolves dozens of these
// on startup, so a single bad Start-menu shortcut must not take the process down.
#pragma once

#include <string>

#include "shell/app_entry.h"

namespace dockforge::shell {

enum class ResolveError {
    None = 0,
    EmptyPath,
    NotFound,
    NotAShortcut,
    LinkLoadFailed,
    NoTarget,
    InvalidAumid,
};

const char* ToString(ResolveError error);

// What an AppsFolder parsing name actually is. The Start menu mixes four shapes in
// one list, and getting this wrong silently drops apps: on this machine 216 entries
// break down as 43 packaged ids, 50 drive paths, 54 shell-namespace children and
// 69 bare desktop app ids.
enum class ParsingKind {
    ShortcutPath,    // "C:\...\Thing.lnk"
    FilePath,        // "C:\...\thing.exe"
    PackagedAppId,   // "Microsoft.WindowsCalculator_8wekyb3d8bbwe!App"
    DesktopAppId,    // "Anysphere.Cursor" - a Win32 app that registered an AUMID
    NamespacePath,   // "{1AC14E77-...}\cmd.exe" - relative to a shell namespace folder
    Unknown,
};

const char* ToString(ParsingKind kind);

ParsingKind ClassifyParsingName(const std::wstring& parsing);

// ---------------------------------------------------------------- pure helpers
// (no COM, no filesystem access - unit tested directly)

// Case-insensitive suffix test for ".lnk".
bool HasShortcutExtension(const std::wstring& path);

// True for a packaged app id: "<PackageFamilyName>!<ApplicationId>".
bool LooksLikeAumid(const std::wstring& text);

// True for anything the shell can launch from AppsFolder: packaged ids and bare
// desktop app ids alike. A bare id has no "!" and no path separator.
bool LooksLikeAppId(const std::wstring& text);

// "C:\Dir\App.exe" -> "App". Used when a shortcut has no display name of its own.
std::wstring TitleFromPath(const std::wstring& path);

// Directory part of a path, without the trailing separator. Empty when the path
// has no directory component.
std::wstring DirectoryPart(const std::wstring& path);

// What to hand IShellItemImageFactory for this entry. Prefers the .lnk's own icon
// location (a shortcut may point at a DLL that holds a better icon than the exe),
// then the target, then the AUMID parsing name.
std::wstring IconSourceFor(const AppEntry& entry);

// ---------------------------------------------------------------- COM helpers

// Expands %VAR% references. Returns the input unchanged when it holds none.
// IShellLinkW::GetPath returns raw paths, so this must run before any use.
std::wstring ExpandEnvironment(const std::wstring& text);

// Resolves a .lnk. On success `out` is fully populated and `error` is None.
ResolveError ResolveShortcut(const std::wstring& lnk_path, AppEntry& out);

// Resolves any path the shell understands: .lnk, .exe, .url, a document.
ResolveError ResolvePath(const std::wstring& path, AppEntry& out);

// Builds an entry for a packaged app. `display_name` must come from the shell
// namespace (AppsFolder enumeration); asking the shell for the name of a bare
// AUMID yields an empty string.
AppEntry MakeUwpEntry(const std::wstring& display_name, const std::wstring& aumid);

// Builds an entry for a Win32 app that registered an AppUserModelID with the Start
// menu instead of exposing a shortcut. It has no executable path of its own.
// Measured on this machine: IApplicationActivationManager::ActivateApplication
// rejects bare ids with E_INVALIDARG, while ShellExecuteEx on
// "shell:AppsFolder\<id>" starts the app, so that string is the launch identity.
AppEntry MakeShellItemEntry(const std::wstring& display_name, const std::wstring& app_id);

// Builds an entry for a child of a shell namespace folder, whose parsing name is
// "{GUID}\name". Such a name is only meaningful relative to its parent, so
// SHCreateItemFromParsingName rejects the bare form; prefixing it with
// "shell:AppsFolder\" makes it resolvable for both icons and ShellExecuteEx.
AppEntry MakeNamespaceEntry(const std::wstring& display_name, const std::wstring& parsing);

// Resolves "shell:AppsFolder\<AUMID>" and fills display_name from the property
// system when the shell has one. Returns InvalidAumid when the package is absent:
// CoCreateInstance(ApplicationActivationManager) accepts ANY AUMID, so it cannot
// be used to validate one.
ResolveError ResolveAumid(const std::wstring& aumid, AppEntry& out);

// Resolves one AppsFolder parsing name into an entry, dispatching on its shape.
// This is the single entry point the scanner uses; keeping the dispatch here means
// the four shapes are classified in one place instead of three.
ResolveError ResolveParsingName(const std::wstring& parsing, const std::wstring& display_name,
                                AppEntry& out);

}  // namespace dockforge::shell
