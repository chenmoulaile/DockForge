// DockForge - folder drill-through browser logic (T-066, ADR-0023).
//
// Contract: CONTRACTS.md puts shell APIs in src/shell; this module is the half
// of the folder browser that never touches a window: enumerate a directory
// through IShellFolder::EnumObjects (the same source Explorer shows, so names
// and icons cannot disagree), sort it deterministically, and map failures to
// statuses the panel can render as friendly placeholders.
//
// The panel (src/dock/folder_panel.*) owns the HWND and the paint; everything
// here is unit-testable without one.
#pragma once

#include "platform/win_common.h"

#include <string>
#include <vector>

namespace dockforge::shell {

// One row of the folder browser panel.
struct FolderEntry {
    // SIGDN_NORMALDISPLAY - what Explorer would show, localized.
    std::wstring display_name;
    // SIGDN_DESKTOPABSOLUTEPARSING. For filesystem items this is a plain path
    // ("C:\...\hello.txt"); the panel uses it to drill down and to resolve
    // icons. Virtual-namespace children can yield non-path strings; callers
    // must treat any value they cannot use as "no icon, not navigable".
    std::wstring full_path;
    bool is_folder = false;
};

// Why an enumeration did not produce a listing. The panel renders one friendly
// placeholder per non-Ok status and must never crash on any of them.
enum class FolderStatus {
    Ok,
    NotFound,      // the path does not exist (ERROR_FILE_NOT_FOUND family)
    AccessDenied,  // the token cannot list the folder
    Failed,        // anything else the shell refused with
};

const char* ToString(FolderStatus status);

struct FolderListing {
    FolderStatus status = FolderStatus::Failed;
    std::vector<FolderEntry> entries;
    // Wall time of the enumeration, for the log. The click path pays this on
    // every open/drill, so it is measured rather than assumed.
    double elapsed_ms = 0.0;
};

// HRESULT -> FolderStatus. Public because the panel also maps probe-time
// failures and a test can pin the mapping for the three codes that matter.
FolderStatus FolderStatusFromHresult(HRESULT hr);

// Enumerates `dir` via SHParseDisplayName -> BindToObject -> EnumObjects +
// IEnumIDList (ADR-0023; spike spikes/T-066_shellfolder_enum). Synchronous and
// blocking: measured 0.1-215ms cold-to-warm, acceptable on a click path, never
// on a frame path. The returned listing is already sorted (SortFolderEntries),
// so a caller cannot forget to. Missing, denied and otherwise failed folders
// return the matching status with an empty entry list instead of throwing.
FolderListing ListFolder(const std::wstring& dir);

// Sorts in place: folders first, then case-insensitive display name ascending,
// ties broken by full path so the order is total and stable across runs.
void SortFolderEntries(std::vector<FolderEntry>& entries);

// The friendly text a panel row (or the whole empty panel body) shows for a
// non-Ok status. Chinese, matching the rest of the user-facing strings.
std::wstring PlaceholderFor(FolderStatus status);

// Back/forward navigation for the panel: a stack of directory paths. Drill
// pushes, Back pops, and popping the root leaves the root (the panel closes on
// that signal instead of going below it).
class FolderNavStack {
  public:
    explicit FolderNavStack(std::wstring root) { stack_.push_back(std::move(root)); }

    // Current directory. Never empty: the stack always holds at least the root.
    const std::wstring& Current() const { return stack_.back(); }
    const std::wstring& Root() const { return stack_.front(); }
    // True when only the root remains.
    bool AtRoot() const { return stack_.size() <= 1; }
    // Enters a subdirectory. Empty names are ignored (a drill into nothing is
    // not a navigation; the caller treats it as a click on the current level).
    void Push(std::wstring dir);
    // Leaves the current directory. Returns false when already at the root - the
    // caller closes the panel on that, it does not go below the pinned folder.
    bool Pop();
    std::size_t Depth() const { return stack_.size(); }

  private:
    std::vector<std::wstring> stack_;
};

}  // namespace dockforge::shell
