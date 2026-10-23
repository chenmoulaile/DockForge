// DockForge - Win32 error/hresult text helpers.
#include "platform/win_common.h"

#include <comdef.h>

namespace dockforge {

std::wstring HResultText(HRESULT hr) {
    wchar_t* buffer = nullptr;
    const DWORD chars = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);

    std::wstring text;
    if (chars && buffer) {
        text.assign(buffer, chars);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                                 text.back() == L' ')) {
            text.pop_back();
        }
    }
    if (buffer) LocalFree(buffer);

    wchar_t code[16];
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
    if (text.empty()) {
        text = code;
    } else {
        text += L" (";
        text += code;
        text += L")";
    }
    return text;
}

}  // namespace dockforge
