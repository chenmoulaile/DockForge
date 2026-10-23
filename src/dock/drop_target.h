// DockForge - OLE drop target for the dock window (T-014, CONTRACTS.md C-16).
//
// Why a hand-written IDropTarget rather than the shell's default: the dock is a
// WS_EX_NOACTIVATE tool window with a SetWindowRgn hit-test shape, and both of those
// interact with OLE in ways the default helper does not cover. The spike
// (spikes/t014_drop) measured the two that matter:
//
//   * A region-clipped window never receives DragEnter outside the region. The dock
//     is trimmed to the plate while idle, so a drop aimed at the magnified row would
//     be invisible to OLE unless the region is widened on entry.
//   * RegisterDragDrop fails with E_OUTOFMEMORY (0x8007000E) after a bare
//     CoInitializeEx, and succeeds only after OleInitialize. src/main.cpp used to
//     call CoInitializeEx; that is why the drop target owns the OLE init decision
//     (see OleSession below) instead of assuming the caller got it right.
#pragma once

#include <windows.h>
#include <ole2.h>
#include <oleidl.h>

#include <functional>
#include <string>
#include <vector>

namespace dockforge::dock {

// Reference-counted wrapper around OleInitialize/OleUninitialize.
//
// Process-wide and idempotent: constructing a second one in the same thread just
// bumps a counter. This exists because the failure it prevents is silent - the
// window appears, the cursor shows a drop hint, and nothing ever arrives.
class OleSession {
  public:
    OleSession();
    ~OleSession();
    OleSession(const OleSession&) = delete;
    OleSession& operator=(const OleSession&) = delete;

    bool ok() const { return ok_; }
    HRESULT result() const { return result_; }

  private:
    bool ok_ = false;
    HRESULT result_ = E_FAIL;
};

// Where the drop target is pointing, in DIP relative to the window's client area.
struct DropPointer {
    bool active = false;      // a drag is currently over the dock
    bool acceptable = false;  // the payload would be accepted if released now
    float x_dip = 0.0f;
    float y_dip = 0.0f;
    int insert_index = 0;     // slot the payload would land in
    int path_count = 0;
};

// The seam between the OLE callback (which runs inside the source app's modal drag
// loop) and the dock. Every call happens on the UI thread and must return fast: the
// source app is blocked until it does.
//
// Plain callbacks rather than a virtual interface because the implementation lives in
// a single function body in main.cpp, where every piece of state it needs is a local.
struct DropSink {
    std::function<void(const DropPointer&)> hover;
    std::function<void()> leave;
    std::function<void(const std::vector<std::wstring>&, int)> commit;
};

class DockDropTarget final : public IDropTarget {
  public:
    DockDropTarget(HWND hwnd, DropSink* sink);
    // Not `override`: IDropTarget has no virtual destructor (it is a COM interface),
    // so the object is only ever destroyed through Release().
    ~DockDropTarget() = default;

    // Registers with OLE. Returns the HRESULT so the caller can log a real failure
    // instead of a "drop silently does nothing" symptom.
    HRESULT Register();
    void Revoke();

    const DropPointer& Pointer() const { return pointer_; }

    // Insertion slot for a pointer at `x_dip`, given the current row centres.
    // Exposed for tests: the same function the OLE callbacks use.
    static int InsertIndexFor(float x_dip, const std::vector<float>& centres_dip);

    // Called by the owner whenever the row geometry changes, so a drag that is
    // already in flight resolves against the current grid rather than the one
    // captured at startup. The centres are the UNMAGNIFIED ones: the dock settles to
    // its rest row while a drag is in progress (see C-16d), so the rest grid is what
    // the user sees under the pointer.
    void SetCentres(std::vector<float> centres_dip);

    // The scale the row geometry was built with. Must come from the same place the
    // layout did (window.Scale()), never from GetDpiForWindow: the two disagree the
    // moment DOCKFORGE_DPI_OVERRIDE is in play, and the symptom is a drop landing two
    // slots away from the caret with nothing in the log to say why. Measured at 96 DPI
    // override on a 192 DPI panel: GetDpiForWindow said 192, the row was laid out at
    // 1.0, and every x was halved (C-10: geometry and DPI must share one source).
    void SetScale(float scale);

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IDropTarget
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt,
                                        DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
    HRESULT STDMETHODCALLTYPE DragLeave() override;
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt,
                                   DWORD* effect) override;

  private:
    void UpdatePointer(POINTL pt);
    static bool ReadHdrop(IDataObject* data, std::vector<std::wstring>& paths);

    HWND hwnd_;
    DropSink* sink_;
    ULONG refs_ = 1;
    bool registered_ = false;
    DropPointer pointer_;
    std::vector<std::wstring> payload_;
    std::vector<float> centres_dip_;
    float scale_ = 0.0f;  // 0 = not told yet, fall back to the window DPI
};

}  // namespace dockforge::dock
