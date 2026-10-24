// DockForge - which windows belong to one dock tile (T-015).
//
// Contract: C-14f/C-13c forbid a second window-matching implementation, and
// C-14h promises per-window indicators "depending on T-015's window list". This
// is that list.
//
// It is deliberately NOT a new matching rule. dock::IdentityOf (C-15a) already
// defines what makes two entries "the same app" - AUMID, else namespace parsing
// name, else path, else display name - and shell::FindAppWindow already applies
// the window half. This function reuses both: the tile's identity is computed
// with dock::IdentityOf, and a window matches when its own identity (AUMID when
// the window reports one, else its executable path) equals it after the same
// lowercasing. Reimplementing the comparison here would be exactly the
// "second matching implementation" C-14f forbids, and would silently diverge the
// first time the identity rules change.
//
// The source HWND for the thumbnail falls out of the same enumeration:
//   * a plain Win32 app's own top-level window is the source;
//   * a packaged (UWP) app's visible top-level window is owned by
//     ApplicationFrameHost.exe, and window_registry.cpp records the real app_pid
//     by walking that frame's children (C-13b). The frame host HWND is what DWM
//     can thumbnail, and thumbnail-ing the frame host shows the app's real
//     content, so AppWindow.hwnd is the source for packaged apps too.
// Both are the AppWindow.hwnd that EnumerateAppWindows already reports, so this
// adds no new enumeration.
#pragma once

#include "shell/app_entry.h"
#include "shell/window_registry.h"

#include <vector>

namespace dockforge::shell {

// Windows that belong to one tile, in enumeration order (z-order, MRU first).
std::vector<AppWindow> WindowsForEntry(const shell::AppEntry& entry,
                                       const std::vector<AppWindow>& windows);

}  // namespace dockforge::shell
