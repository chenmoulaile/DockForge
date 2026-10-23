// DockForge - shared Win32 prelude.
// Keeps every translation unit on the same macro surface so COM/Win32
// signatures never drift between modules.
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <dwmapi.h>
#include <d2d1_3.h>
#include <dwrite_3.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace dockforge {

using Microsoft::WRL::ComPtr;

// Throws nothing; returns L"" for S_OK-equivalent codes we cannot map.
std::wstring HResultText(HRESULT hr);

// Simple RAII scope guard.
template <typename Fn>
struct ScopeGuard {
    Fn fn;
    bool armed = true;
    ~ScopeGuard() { if (armed) fn(); }
};

template <typename Fn>
ScopeGuard<Fn> MakeGuard(Fn fn) { return ScopeGuard<Fn>{fn, true}; }

}  // namespace dockforge
