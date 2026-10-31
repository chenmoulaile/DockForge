// Enumerates the notification-area window tree so the probe knows where the
// buttons actually live. The classic recipe (Shell_TrayWnd -> TrayNotifyWnd ->
// SysPager -> ToolbarWindow32) is from Windows 7 and may not survive the XAML
// tray on Windows 11.
//
// Build:
//   g++ -std=c++17 -O2 -o tray_tree.exe tray_tree.cpp -luser32 -lcomctl32

#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <commctrl.h>

#include <cstdio>

namespace {

void DumpChildren(HWND parent, int depth) {
    HWND child = nullptr;
    int index = 0;
    while ((child = FindWindowExW(parent, child, nullptr, nullptr)) != nullptr) {
        wchar_t cls[256] = {};
        wchar_t title[256] = {};
        GetClassNameW(child, cls, 256);
        GetWindowTextW(child, title, 256);
        RECT rect{};
        GetWindowRect(child, &rect);
        const int buttons = static_cast<int>(SendMessageW(child, TB_BUTTONCOUNT, 0, 0));
        std::printf("%*s[%d] hwnd=%p class=%ls title=%ls rect=%ld,%ld,%ld,%ld visible=%d buttons=%d\n",
                    depth * 2, "", index, static_cast<void*>(child), cls, title, rect.left, rect.top,
                    rect.right, rect.bottom, IsWindowVisible(child) ? 1 : 0, buttons);
        if (depth < 6) DumpChildren(child, depth + 1);
        ++index;
    }
}

}  // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::printf("system dpi=%d\n", GetDeviceCaps(GetDC(nullptr), LOGPIXELSX));

    const wchar_t* roots[] = {L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"NotifyIconOverflowWindow",
                              L"TopLevelWindowForOverflowXamlIsland"};
    for (const wchar_t* name : roots) {
        HWND root = FindWindowW(name, nullptr);
        std::printf("\n=== root %ls = %p ===\n", name, static_cast<void*>(root));
        if (root != nullptr) DumpChildren(root, 1);
    }
    return 0;
}
