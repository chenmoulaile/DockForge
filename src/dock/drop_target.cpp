#include "dock/drop_target.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>

#include "core/log.h"
#include "core/paths.h"
#include "shell/drop_resolver.h"

namespace dockforge::dock {
namespace {

int g_ole_refs = 0;

}  // namespace

OleSession::OleSession() {
    // OleInitialize, not CoInitializeEx. Measured (spikes/t014_drop, --ole-mode
    // comparison): a bare CoInitializeEx succeeds and then RegisterDragDrop returns
    // 0x8007000E (E_OUTOFMEMORY), which reads like a memory problem and is actually
    // "OLE was not initialised". OleInitialize cannot be nested with a mode that
    // disables OLE1DDE, so the process must not have called CoInitializeEx first.
    if (g_ole_refs++ > 0) {
        ok_ = true;
        result_ = S_FALSE;
        return;
    }
    result_ = OleInitialize(nullptr);
    ok_ = SUCCEEDED(result_);
    if (!ok_) {
        DF_ERROR("dock", "OleInitialize failed: 0x%08lX", static_cast<unsigned long>(result_));
    }
}

OleSession::~OleSession() {
    if (g_ole_refs > 0 && --g_ole_refs == 0 && SUCCEEDED(result_)) OleUninitialize();
}

DockDropTarget::DockDropTarget(HWND hwnd, DropSink* sink) : hwnd_(hwnd), sink_(sink) {}

HRESULT DockDropTarget::Register() {
    if (registered_) return S_OK;
    const HRESULT hr = RegisterDragDrop(hwnd_, this);
    if (SUCCEEDED(hr)) {
        registered_ = true;
    } else {
        DF_ERROR("dock", "RegisterDragDrop failed: 0x%08lX", static_cast<unsigned long>(hr));
    }
    return hr;
}

void DockDropTarget::Revoke() {
    if (!registered_) return;
    RevokeDragDrop(hwnd_);
    registered_ = false;
}

void DockDropTarget::SetCentres(std::vector<float> centres_dip) {
    centres_dip_ = std::move(centres_dip);
}

void DockDropTarget::SetScale(float scale) {
    scale_ = scale > 0.0f ? scale : 0.0f;
}

int DockDropTarget::InsertIndexFor(float x_dip, const std::vector<float>& centres_dip) {
    if (centres_dip.empty()) return 0;
    // Count the centres the pointer has passed. `>=` rather than `>` for the same
    // reason as the internal reorder (C-15d): a pointer resting exactly on a centre is
    // on that tile, not before it.
    int passed = 0;
    for (float centre : centres_dip) {
        if (x_dip >= centre) ++passed;
    }
    return std::clamp(passed, 0, static_cast<int>(centres_dip.size()));
}

void DockDropTarget::UpdatePointer(POINTL pt) {
    // OLE hands the pointer in screen pixels. The dock lays out in DIP, so the
    // conversion happens once, here. The scale comes from SetScale - i.e. from
    // window.Scale(), the same value the row geometry was built with - and only falls
    // back to the window DPI when the owner has not told us yet. Reading
    // GetDpiForWindow directly is wrong: under DOCKFORGE_DPI_OVERRIDE the window keeps
    // its real DPI while the row is laid out at the override, and every x comes out
    // scaled by the ratio between them (C-10: geometry and DPI share one source).
    POINT client{pt.x, pt.y};
    ScreenToClient(hwnd_, &client);
    const UINT dpi = GetDpiForWindow(hwnd_);
    const float scale = scale_ > 0.0f
                            ? scale_
                            : (dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f);

    pointer_.x_dip = static_cast<float>(client.x) / scale;
    pointer_.y_dip = static_cast<float>(client.y) / scale;
    pointer_.path_count = static_cast<int>(payload_.size());
    pointer_.insert_index = InsertIndexFor(pointer_.x_dip, centres_dip_);
    // The slot depends on three numbers that all come from different places (screen
    // point, row scale, geometry), so a wrong slot is otherwise indistinguishable from
    // a wrong caret. Logged at trace level because DragOver fires dozens of times a
    // second.
    DF_TRACE("dock", "drop pointer screen=(%ld,%ld) client=(%ld,%ld) dpi=%u scale=%.3f x=%.1fdip "
                     "slots=%zu insert=%d",
             pt.x, pt.y, client.x, client.y, dpi, static_cast<double>(scale),
             static_cast<double>(pointer_.x_dip), centres_dip_.size(), pointer_.insert_index);
}

HRESULT DockDropTarget::QueryInterface(REFIID riid, void** out) {
    if (out == nullptr) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG DockDropTarget::AddRef() { return ++refs_; }

ULONG DockDropTarget::Release() {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
}

bool DockDropTarget::ReadHdrop(IDataObject* data, std::vector<std::wstring>& paths) {
    paths.clear();
    if (data == nullptr) return false;

    FORMATETC fmt{};
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    if (FAILED(data->GetData(&fmt, &medium))) return false;

    auto drop = static_cast<HDROP>(medium.hGlobal);
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) continue;
        std::wstring path(static_cast<std::size_t>(len) + 1, L'\0');
        const UINT written = DragQueryFileW(drop, i, path.data(), len + 1);
        path.resize(written);
        paths.push_back(std::move(path));
    }
    ReleaseStgMedium(&medium);
    return true;
}

HRESULT DockDropTarget::DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    payload_.clear();
    const bool has_hdrop = ReadHdrop(data, payload_);
    // Classification happens here, once per drag, not on every DragOver: the answer
    // cannot change while the same data object is in flight, and a drag fires dozens
    // of DragOver calls per second.
    const shell::DropKind kind =
        has_hdrop ? shell::ClassifyDroppedPaths(payload_) : shell::DropKind::None;
    pointer_.active = true;
    pointer_.acceptable = shell::IsDockable(kind);
    UpdatePointer(pt);

    DF_INFO("dock", "drop enter: paths=%d kind=%s acceptable=%d", pointer_.path_count,
            shell::ToString(kind), pointer_.acceptable ? 1 : 0);
    for (const auto& path : payload_) {
        DF_DEBUG("dock", "  drop path: %s", core::WideToUtf8(path).c_str());
    }

    if (sink_ != nullptr && sink_->hover) sink_->hover(pointer_);
    if (effect != nullptr) *effect = pointer_.acceptable ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT DockDropTarget::DragOver(DWORD keys, POINTL pt, DWORD* effect) {
    if (!pointer_.active) return S_OK;
    UpdatePointer(pt);
    if (sink_ != nullptr && sink_->hover) sink_->hover(pointer_);
    // The effect must be reported every time, not only on entry: the source uses it
    // to pick the cursor, and a NONE here is what tells the user "not here".
    if (effect != nullptr) *effect = pointer_.acceptable ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
}

HRESULT DockDropTarget::DragLeave() {
    DF_INFO("dock", "drop leave");
    pointer_ = DropPointer{};
    payload_.clear();
    if (sink_ != nullptr && sink_->leave) sink_->leave();
    return S_OK;
}

HRESULT DockDropTarget::Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) {
    UpdatePointer(pt);
    const bool acceptable = pointer_.acceptable;
    const int insert_index = pointer_.insert_index;
    // Copy before clearing: the sink may start work that outlives this call, and the
    // OLE data object is only valid until Drop returns.
    std::vector<std::wstring> paths = payload_;
    pointer_ = DropPointer{};
    payload_.clear();

    if (effect != nullptr) *effect = acceptable ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (!acceptable) {
        DF_INFO("dock", "drop rejected: %zu paths were not dockable", paths.size());
        if (sink_ != nullptr && sink_->leave) sink_->leave();
        return S_OK;
    }

    DF_INFO("dock", "drop commit: %zu paths at insert=%d", paths.size(), insert_index);
    if (sink_ != nullptr && sink_->commit) sink_->commit(paths, insert_index);
    return S_OK;
}

}  // namespace dockforge::dock
