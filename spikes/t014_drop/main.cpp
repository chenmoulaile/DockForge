// T-014 spike: does an OLE drop target work on a dock-shaped window
// (topmost + no-activate + tool window + no redirection bitmap + region-clipped),
// and what does a real OLE drag actually deliver?
//
// Build:
//   cmake -S spikes/t014_drop -B spikes/t014_drop/build -G "MinGW Makefiles" ...
//
// Modes:
//   --mode=target --hold-ms=8000      just register and wait for a human drag
//   --mode=both   --hold-ms=12000     source + target in one process, self-driven
//   --mode=register                    register only, print the HRESULT, exit
//
// `both` is the autonomous path: a driver thread synthesises the mouse input and
// the main thread's modal DoDragDrop loop does the OLE handshake.

#include <windows.h>
#include <windowsx.h>
#include <ole2.h>
#include <oleidl.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

HANDLE g_log = INVALID_HANDLE_VALUE;

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    std::string line(buf);
    line.push_back('\n');
    if (g_log == INVALID_HANDLE_VALUE) {
        std::fputs(line.c_str(), stdout);
        std::fflush(stdout);
        return;
    }
    DWORD written = 0;
    WriteFile(g_log, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

std::string ToUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return std::string();
    const int need = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return std::string();
    std::string out(static_cast<std::size_t>(need - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), need, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Minimal CF_HDROP data object.
// ---------------------------------------------------------------------------
class FileDataObject final : public IDataObject {
public:
    explicit FileDataObject(std::vector<std::wstring> paths) : paths_(std::move(paths)) {
        BuildDropFiles();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (format == nullptr || medium == nullptr) return E_POINTER;
        if (!IsHdrop(format)) return DV_E_FORMATETC;
        const HGLOBAL copy = DupGlobal(drop_);
        if (copy == nullptr) return E_OUTOFMEMORY;
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = copy;
        medium->pUnkForRelease = nullptr;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
        return DATA_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
        if (format == nullptr) return E_POINTER;
        return IsHdrop(format) ? S_OK : DV_E_FORMATETC;
    }

    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out != nullptr) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC fmt = Format();
        return SHCreateStdEnumFmtEtc(1, &fmt, out);
    }

    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
        return OLE_E_ADVISENOTSUPPORTED;
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override {
        if (out != nullptr) *out = nullptr;
        return OLE_E_ADVISENOTSUPPORTED;
    }

private:
    ~FileDataObject() {
        if (drop_ != nullptr) GlobalFree(drop_);
    }

    static FORMATETC Format() {
        FORMATETC fmt{};
        fmt.cfFormat = CF_HDROP;
        fmt.ptd = nullptr;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        return fmt;
    }

    static bool IsHdrop(const FORMATETC* format) {
        return format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) != 0 &&
               format->dwAspect == DVASPECT_CONTENT;
    }

    static HGLOBAL DupGlobal(HGLOBAL source) {
        const SIZE_T size = GlobalSize(source);
        if (size == 0) return nullptr;
        HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
        if (copy == nullptr) return nullptr;
        const void* from = GlobalLock(source);
        void* to = GlobalLock(copy);
        if (from == nullptr || to == nullptr) {
            if (to != nullptr) GlobalUnlock(copy);
            if (from != nullptr) GlobalUnlock(source);
            GlobalFree(copy);
            return nullptr;
        }
        std::memcpy(to, from, size);
        GlobalUnlock(copy);
        GlobalUnlock(source);
        return copy;
    }

    void BuildDropFiles() {
        std::size_t chars = 1;
        for (const auto& path : paths_) chars += path.size() + 1;
        const std::size_t bytes = sizeof(DROPFILES) + chars * sizeof(wchar_t);
        drop_ = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (drop_ == nullptr) return;
        auto* files = static_cast<DROPFILES*>(GlobalLock(drop_));
        if (files == nullptr) return;
        files->pFiles = sizeof(DROPFILES);
        files->fWide = TRUE;
        auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<std::byte*>(files) + sizeof(DROPFILES));
        for (const auto& path : paths_) {
            std::memcpy(cursor, path.c_str(), path.size() * sizeof(wchar_t));
            cursor += path.size();
            *cursor++ = L'\0';
        }
        *cursor = L'\0';
        GlobalUnlock(drop_);
    }

    std::vector<std::wstring> paths_;
    HGLOBAL drop_ = nullptr;
    ULONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Minimal IDropSource. DoDragDrop requires one; passing nullptr returns
// E_INVALIDARG (0x80070057) before any drag begins.
// ---------------------------------------------------------------------------
class MouseDropSource final : public IDropSource {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape) {
            Log("source: QueryContinueDrag -> CANCEL (escape)");
            return DRAGDROP_S_CANCEL;
        }
        if ((keys & MK_LBUTTON) == 0) {
            Log("source: QueryContinueDrag -> DROP (button released)");
            return DRAGDROP_S_DROP;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

private:
    ULONG refs_ = 1;
};
// ---------------------------------------------------------------------------
// Drop target that only logs.
// ---------------------------------------------------------------------------
class LoggingDropTarget final : public IDropTarget {
public:
    explicit LoggingDropTarget(HWND hwnd) : hwnd_(hwnd) {}
    void SetExpandOnEnter(bool value) { expand_on_enter_ = value; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt,
                                        DWORD* effect) override {
        POINT screen{pt.x, pt.y};
        ScreenToClient(hwnd_, &screen);
        Log("DragEnter screen=(%ld,%ld) client=(%ld,%ld) keys=0x%08lX effect_in=0x%08lX", pt.x, pt.y,
            screen.x, screen.y, keys, effect != nullptr ? *effect : 0);
        LogFormats(data);
        const bool acceptable = OffersHdrop(data);
        Log("  acceptable=%d", acceptable ? 1 : 0);
        if (effect != nullptr) *effect = acceptable ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        entered_ = true;
        if (expand_on_enter_ && !expanded_) {
            expanded_ = true;
            RECT rc{};
            GetWindowRect(hwnd_, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            HRGN full = CreateRectRgn(0, 0, w, h);
            const int ok = SetWindowRgn(hwnd_, full, TRUE);
            Log("  expand_on_enter: SetWindowRgn(full %dx%d) -> %d", w, h, ok);
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        POINT screen{pt.x, pt.y};
        ScreenToClient(hwnd_, &screen);
        ++over_count_;
        if (over_count_ <= 3 || over_count_ % 20 == 0) {
            Log("DragOver  #%d screen=(%ld,%ld) client=(%ld,%ld) keys=0x%08lX", over_count_, pt.x,
                pt.y, screen.x, screen.y, keys);
        }
        if (effect != nullptr) *effect = DROPEFFECT_COPY;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        Log("DragLeave over_count=%d", over_count_);
        entered_ = false;
        expanded_ = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        POINT screen{pt.x, pt.y};
        ScreenToClient(hwnd_, &screen);
        Log("Drop screen=(%ld,%ld) client=(%ld,%ld) keys=0x%08lX over_count=%d", pt.x, pt.y, screen.x,
            screen.y, keys, over_count_);
        if (effect != nullptr) *effect = DROPEFFECT_COPY;
        DumpHdrop(data);
        entered_ = false;
        return S_OK;
    }

private:
    static bool OffersHdrop(IDataObject* data) {
        if (data == nullptr) return false;
        FORMATETC fmt{};
        fmt.cfFormat = CF_HDROP;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        return data->QueryGetData(&fmt) == S_OK;
    }

    static void LogFormats(IDataObject* data) {
        IEnumFORMATETC* enum_formats = nullptr;
        if (data == nullptr || FAILED(data->EnumFormatEtc(DATADIR_GET, &enum_formats)) ||
            enum_formats == nullptr) {
            Log("  formats=<none>");
            return;
        }
        FORMATETC fmt{};
        ULONG fetched = 0;
        while (enum_formats->Next(1, &fmt, &fetched) == S_OK && fetched == 1) {
            wchar_t name[128] = {};
            if (GetClipboardFormatNameW(fmt.cfFormat, name, 128) == 0) {
                wsprintfW(name, L"#%u", fmt.cfFormat);
            }
            Log("  format cf=%u name=%s tymed=0x%08lX aspect=0x%08lX", fmt.cfFormat,
                ToUtf8(name).c_str(), fmt.tymed, fmt.dwAspect);
        }
        enum_formats->Release();
    }

    static void DumpHdrop(IDataObject* data) {
        if (data == nullptr) return;
        FORMATETC fmt{};
        fmt.cfFormat = CF_HDROP;
        fmt.dwAspect = DVASPECT_CONTENT;
        fmt.lindex = -1;
        fmt.tymed = TYMED_HGLOBAL;
        STGMEDIUM medium{};
        if (FAILED(data->GetData(&fmt, &medium))) {
            Log("  GetData(CF_HDROP) failed");
            return;
        }
        auto drop = static_cast<HDROP>(medium.hGlobal);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        Log("  hdrop_count=%u", count);
        for (UINT i = 0; i < count; ++i) {
            wchar_t path[MAX_PATH] = {};
            const UINT len = DragQueryFileW(drop, i, path, MAX_PATH);
            const DWORD attrs = GetFileAttributesW(path);
            Log("  path[%u] len=%u attrs=0x%08lX %s", i, len, attrs, ToUtf8(path).c_str());
        }
        POINT drop_point{};
        if (DragQueryPoint(drop, &drop_point)) {
            Log("  query_point=(%ld,%ld)", drop_point.x, drop_point.y);
        }
        ReleaseStgMedium(&medium);
    }

    HWND hwnd_;
    ULONG refs_ = 1;
    bool entered_ = false;
    bool expand_on_enter_ = false;
    bool expanded_ = false;
    int over_count_ = 0;
};

// ---------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------
constexpr wchar_t kTargetClass[] = L"Spike014Target";
constexpr wchar_t kSourceClass[] = L"Spike014Source";

HWND g_target = nullptr;
HWND g_source = nullptr;
LoggingDropTarget* g_target_impl = nullptr;
std::vector<std::wstring> g_payload;
RECT g_target_rect{};
bool g_drag_started = false;
HANDLE g_driver_thread = nullptr;
int g_drop_y_percent = 80;
int g_dwell_ms = 300;
int g_end_y_percent = -1;
bool g_has_explicit_target = false;
POINT g_explicit_target{};

LRESULT CALLBACK TargetProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(RGB(13, 15, 18));
        FillRect(reinterpret_cast<HDC>(wp), &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SourceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_LBUTTONDOWN) {
        Log("source: WM_LBUTTONDOWN (arming)");
        return 0;
    }
    if (msg == WM_MOUSEMOVE) {
        if ((wp & MK_LBUTTON) != 0 && !g_drag_started) {
            g_drag_started = true;
            Log("source: WM_MOUSEMOVE + MK_LBUTTON -> DoDragDrop (modal)");
            auto* data = new FileDataObject(g_payload);
            auto* source = new MouseDropSource();
            DWORD effect = DROPEFFECT_COPY;
            const HRESULT hr = DoDragDrop(data, source, DROPEFFECT_COPY, &effect);
            Log("source: DoDragDrop hr=0x%08lX effect=0x%08lX", static_cast<unsigned>(hr), effect);
            source->Release();
            data->Release();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool MakeWindowClass(const wchar_t* name, WNDPROC proc) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = name;
    return RegisterClassExW(&wc) != 0;
}

HWND CreateDockShapedTarget(int x, int y, int w, int h, bool apply_region) {
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        kTargetClass, L"spike target", WS_POPUP, x, y, w, h, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    if (hwnd == nullptr) return nullptr;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    GetWindowRect(hwnd, &g_target_rect);
    Log("target: hwnd=%p rect=%ld,%ld %ldx%ld", static_cast<void*>(hwnd), g_target_rect.left,
        g_target_rect.top, g_target_rect.right - g_target_rect.left,
        g_target_rect.bottom - g_target_rect.top);
    if (apply_region) {
        const int width = g_target_rect.right - g_target_rect.left;
        const int height = g_target_rect.bottom - g_target_rect.top;
        // Mirror C-12: only the bottom plate strip is hit-testable.
        HRGN region = CreateRectRgn(0, height * 2 / 5, width, height);
        const int ok = SetWindowRgn(hwnd, region, TRUE);
        Log("target: SetWindowRgn(bottom 3/5) -> %d", ok);
    } else {
        Log("target: no region applied");
    }
    return hwnd;
}

DWORD WINAPI DriverThread(LPVOID) {
    // Wait for the source window to be ready and for the pump to be live.
    Sleep(700);
    RECT src{};
    GetWindowRect(g_source, &src);
    const POINT from{(src.left + src.right) / 2, (src.top + src.bottom) / 2};
    POINT to{};
    if (g_has_explicit_target) {
        to = g_explicit_target;
    } else {
        to = POINT{(g_target_rect.left + g_target_rect.right) / 2,
                   g_target_rect.top + (g_target_rect.bottom - g_target_rect.top) * g_drop_y_percent / 100};
    }
    Log("driver: from=(%ld,%ld) to=(%ld,%ld)", from.x, from.y, to.x, to.y);

    // The drag only starts if the source owns the foreground; Windows delivers
    // the button-down to it but drops the follow-up moves otherwise.
    SetForegroundWindow(g_source);
    Sleep(200);
    SetCursorPos(from.x, from.y);
    Sleep(150);
    INPUT down[1] = {};
    down[0].type = INPUT_MOUSE;
    down[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, down, sizeof(INPUT));
    Sleep(150);
    // A small jiggle guarantees at least one WM_MOUSEMOVE with MK_LBUTTON, which
    // is what arms DoDragDrop in SourceProc.
    for (int jig = 0; jig < 3; ++jig) {
        SetCursorPos(from.x + (jig % 2 == 0 ? 3 : -3), from.y);
        Sleep(60);
    }
    SetCursorPos(from.x, from.y);
    Sleep(60);

    constexpr int kSteps = 20;
    for (int i = 1; i <= kSteps; ++i) {
        SetCursorPos(from.x + (to.x - from.x) * i / kSteps, from.y + (to.y - from.y) * i / kSteps);
        Sleep(45);
    }
    if (g_end_y_percent >= 0 && !g_has_explicit_target) {
        const int endY = g_target_rect.top + (g_target_rect.bottom - g_target_rect.top) * g_end_y_percent / 100;
        Log("driver: second leg to y=%d", endY);
        constexpr int kSteps2 = 12;
        for (int i = 1; i <= kSteps2; ++i) {
            SetCursorPos(to.x, to.y + (endY - to.y) * i / kSteps2);
            Sleep(45);
        }
    }
    Sleep(g_dwell_ms);
    INPUT up[1] = {};
    up[0].type = INPUT_MOUSE;
    up[0].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, up, sizeof(INPUT));
    Log("driver: released");
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    std::string mode = "target";
    int hold_ms = 8000;
    bool apply_region = true;
    int drop_y_percent = 80;
    int end_y_percent = -1;
    bool expand_on_enter = false;
    std::string ole_mode = "oleinit";
    std::wstring out_path;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg.rfind(L"--mode=", 0) == 0) {
            mode = ToUtf8(arg.c_str() + 7);
        } else if (arg.rfind(L"--hold-ms=", 0) == 0) {
            hold_ms = _wtoi(arg.c_str() + 10);
        } else if (arg.rfind(L"--payload=", 0) == 0) {
            g_payload.push_back(arg.substr(10));
        } else if (arg.rfind(L"--out=", 0) == 0) {
            out_path = arg.substr(6);
        } else if (arg.rfind(L"--drop-y-percent=", 0) == 0) {
            drop_y_percent = _wtoi(arg.c_str() + 17);
        } else if (arg.rfind(L"--end-y-percent=", 0) == 0) {
            end_y_percent = _wtoi(arg.c_str() + 16);
        } else if (arg.rfind(L"--to=", 0) == 0) {
            const std::wstring value = arg.substr(5);
            const auto comma = value.find(L",");
            if (comma != std::wstring::npos) {
                g_has_explicit_target = true;
                g_explicit_target.x = _wtoi(value.substr(0, comma).c_str());
                g_explicit_target.y = _wtoi(value.substr(comma + 1).c_str());
            }
        } else if (arg.rfind(L"--ole-mode=", 0) == 0) {
            ole_mode = ToUtf8(arg.c_str() + 11);
        } else if (arg == L"--expand-on-enter") {
            expand_on_enter = true;
        } else if (arg.rfind(L"--dwell-ms=", 0) == 0) {
            g_dwell_ms = _wtoi(arg.c_str() + 11);
        } else if (arg == L"--no-region") {
            apply_region = false;
        }
    }
    if (!out_path.empty()) {
        g_log = CreateFileW(out_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    g_drop_y_percent = drop_y_percent;
    g_end_y_percent = end_y_percent;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HRESULT ole = E_FAIL;
    if (ole_mode == "coinitapt") {
        ole = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Log("CoInitializeEx(APT) -> hr=0x%08lX", static_cast<unsigned>(ole));
    } else if (ole_mode == "coinitex") {
        // Same flags as src/main.cpp: OLE1DDE disabled, STA.
        ole = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        Log("CoInitializeEx(APT|DISABLE_OLE1DDE) -> hr=0x%08lX", static_cast<unsigned>(ole));
    } else {
        ole = OleInitialize(nullptr);
        Log("OleInitialize -> hr=0x%08lX", static_cast<unsigned>(ole));
    }
    Log("mode=%s payload_count=%d region=%d drop_y=%d%%", mode.c_str(),
        static_cast<int>(g_payload.size()), apply_region ? 1 : 0, g_drop_y_percent);
    Log("expand_on_enter=%d", expand_on_enter ? 1 : 0);
    for (std::size_t i = 0; i < g_payload.size(); ++i) {
        Log("  payload[%d] attrs=0x%08lX %s", static_cast<int>(i), GetFileAttributesW(g_payload[i].c_str()),
            ToUtf8(g_payload[i].c_str()).c_str());
    }

    int code = 0;
    if (!MakeWindowClass(kTargetClass, TargetProc)) {
        Log("FAILED RegisterClassExW(target) err=%lu", GetLastError());
        code = 10;
    } else if (!MakeWindowClass(kSourceClass, SourceProc)) {
        Log("FAILED RegisterClassExW(source) err=%lu", GetLastError());
        code = 11;
    } else if (mode == "drag") {
        // Source-only process: the drop target lives in another process, which is
        // what a real explorer.exe drag looks like to the target.
        g_source = CreateWindowExW(WS_EX_TOPMOST, kSourceClass, L"spike source",
                                   WS_OVERLAPPEDWINDOW, 60, 400, 320, 200, nullptr, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        if (g_source == nullptr) {
            Log("FAILED CreateWindowExW(source) err=%lu", GetLastError());
            code = 15;
        } else {
            ShowWindow(g_source, SW_SHOW);
            UpdateWindow(g_source);
            RECT src{};
            GetWindowRect(g_source, &src);
            Log("source: hwnd=%p rect=%ld,%ld %ldx%ld", static_cast<void*>(g_source), src.left,
                src.top, src.right - src.left, src.bottom - src.top);
            g_driver_thread = CreateThread(nullptr, 0, DriverThread, nullptr, 0, nullptr);
        }
    } else {
        // Target sits low on the screen so the driver can drag down into it.
        g_target = CreateDockShapedTarget(600, 1500, 800, 200, apply_region);
        if (g_target == nullptr) {
            Log("FAILED CreateWindowExW(target) err=%lu", GetLastError());
            code = 12;
        } else {
            g_target_impl = new LoggingDropTarget(g_target);
            g_target_impl->SetExpandOnEnter(expand_on_enter);
            const HRESULT hr = RegisterDragDrop(g_target, g_target_impl);
            Log("target: RegisterDragDrop -> hr=0x%08lX%s", static_cast<unsigned>(hr),
                hr == S_OK ? " (S_OK)" : "");
            if (hr != S_OK) code = 13;

            if (code == 0 && mode == "both") {
                g_source = CreateWindowExW(WS_EX_TOPMOST, kSourceClass, L"spike source",
                                           WS_OVERLAPPEDWINDOW, 60, 400, 320, 200, nullptr, nullptr,
                                           GetModuleHandleW(nullptr), nullptr);
                if (g_source == nullptr) {
                    Log("FAILED CreateWindowExW(source) err=%lu", GetLastError());
                    code = 14;
                } else {
                    ShowWindow(g_source, SW_SHOW);
                    UpdateWindow(g_source);
                    RECT src{};
                    GetWindowRect(g_source, &src);
                    Log("source: hwnd=%p rect=%ld,%ld %ldx%ld", static_cast<void*>(g_source),
                        src.left, src.top, src.right - src.left, src.bottom - src.top);
                    g_driver_thread = CreateThread(nullptr, 0, DriverThread, nullptr, 0, nullptr);
                }
            }
        }
    }

    if (code == 0) {
        const ULONGLONG start = GetTickCount64();
        while (GetTickCount64() - start < static_cast<ULONGLONG>(hold_ms)) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    hold_ms = 0;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            Sleep(5);
        }
    }

    if (g_driver_thread != nullptr) {
        WaitForSingleObject(g_driver_thread, 3000);
        CloseHandle(g_driver_thread);
    }
    if (g_target != nullptr) {
        RevokeDragDrop(g_target);
        DestroyWindow(g_target);
    }
    if (g_target_impl != nullptr) {
        g_target_impl->Release();
        g_target_impl = nullptr;
    }
    if (g_source != nullptr) DestroyWindow(g_source);
    OleUninitialize();
    Log("exit code=%d", code);
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    return code;
}
