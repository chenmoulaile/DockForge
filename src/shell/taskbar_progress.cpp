// DockForge - taskbar progress reading, implementation (T-067).
//
// See taskbar_progress.h for the spike evidence behind every design choice
// here; the one that shapes this file most is that the window-property read
// path measured dead, so everything flows from the MSAA white-list scan.
#include "shell/taskbar_progress.h"

#include "core/log.h"

#include <oleacc.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace dockforge::shell {
namespace {

std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

std::wstring BasenameLower(const std::wstring& path) {
    const std::wstring lower = ToLower(path);
    const std::size_t slash = lower.find_last_of(L"/\\");
    return slash == std::wstring::npos ? lower : lower.substr(slash + 1);
}

double NowMs() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

// ---- MSAA walk (ported from spikes/T-067_progress_probe, productised) -------

constexpr int kMaxHwndDepth = 6;
constexpr int kMaxAccessibleDepth = 6;
constexpr long kMaxAccessibleChildren = 256;

// Depth-first search for the first ROLE_SYSTEM_PROGRESSBAR under `acc`.
// Returns its percentage text via value_out (a BSTR owned by the caller).
bool FindProgressBar(IAccessible* acc, int depth, BSTR* value_out) {
    if (!acc || depth > kMaxAccessibleDepth) return false;
    VARIANT self;
    self.vt = VT_I4;
    self.lVal = CHILDID_SELF;

    VARIANT role;
    VariantInit(&role);
    const HRESULT role_hr = acc->get_accRole(self, &role);
    const bool is_progress =
        SUCCEEDED(role_hr) && role.vt == VT_I4 && role.lVal == ROLE_SYSTEM_PROGRESSBAR;
    if (is_progress) {
        acc->get_accValue(self, value_out);
        VariantClear(&role);
        return *value_out != nullptr;
    }
    VariantClear(&role);

    long count = 0;
    if (FAILED(acc->get_accChildCount(&count)) || count <= 0 || count > kMaxAccessibleChildren) {
        return false;
    }
    VARIANT* children = static_cast<VARIANT*>(CoTaskMemAlloc(sizeof(VARIANT) * count));
    if (!children) return false;
    long got = 0;
    const HRESULT enum_hr = AccessibleChildren(acc, 0, count, children, &got);
    bool found = false;
    if (SUCCEEDED(enum_hr)) {
        for (long i = 0; i < got && !found; ++i) {
            if (children[i].vt != VT_DISPATCH || !children[i].pdispVal) continue;
            IAccessible* child = nullptr;
            if (SUCCEEDED(children[i].pdispVal->QueryInterface(IID_IAccessible,
                                                              reinterpret_cast<void**>(&child)))) {
                found = FindProgressBar(child, depth + 1, value_out);
                child->Release();
            }
        }
    }
    for (long i = 0; i < got; ++i) VariantClear(&children[i]);
    CoTaskMemFree(children);
    return found;
}

// Reads the progress percentage from one window and its child HWND tree. The
// DirectUI content exposes its accessible tree on an embedded child window, not
// on the frame - that measured difference is why the walk covers every HWND.
float ReadProgressValue(HWND hwnd, int depth) {
    if (!hwnd || depth > kMaxHwndDepth) return -1.0f;
    float best = -1.0f;
    IAccessible* acc = nullptr;
    if (SUCCEEDED(AccessibleObjectFromWindow(hwnd, OBJID_WINDOW, IID_IAccessible,
                                             reinterpret_cast<void**>(&acc))) &&
        acc) {
        BSTR value = nullptr;
        if (FindProgressBar(acc, 0, &value)) {
            best = ParseProgressPercent(value);
            SysFreeString(value);
        }
        acc->Release();
    }
    if (best >= 0.0f) return best;
    HWND child = nullptr;
    while ((child = FindWindowExW(hwnd, child, nullptr, nullptr)) != nullptr) {
        const float sub = ReadProgressValue(child, depth + 1);
        if (sub >= 0.0f) return sub;
    }
    return -1.0f;
}

struct ScanContext {
    std::vector<HWND> matches;
};

// Free-function twin of CopyDialogMsaaReader::IsWhiteListedClass so the
// EnumWindows callback (a plain function pointer) can reach the check.
bool IsWhiteListedClassImpl(const wchar_t* class_name) {
    for (const ProgressWhiteListEntry& entry : kProgressWhiteList) {
        if (wcscmp(class_name, entry.window_class) == 0) return true;
    }
    return false;
}

BOOL CALLBACK CollectWhiteListed(HWND hwnd, LPARAM lp) {
    auto* ctx = reinterpret_cast<ScanContext*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    WCHAR class_name[64] = L"";
    if (GetClassNameW(hwnd, class_name, 64) == 0) return TRUE;
    if (IsWhiteListedClassImpl(class_name)) ctx->matches.push_back(hwnd);
    return TRUE;
}

}  // namespace

// ---- pure helpers -----------------------------------------------------------

ProgressState ProgressStateFromFlags(int tbpf_flags) {
    // TBPF_* values from shobjidl (spelled out so this file does not drag the
    // whole shell COM surface into every test translation unit).
    constexpr int kTbpfNormal = 0x1;
    constexpr int kTbpfIndeterminate = 0x2;
    constexpr int kTbpfError = 0x4;
    constexpr int kTbpfPaused = 0x8;
    if (tbpf_flags & kTbpfError) return ProgressState::Error;
    if (tbpf_flags & kTbpfPaused) return ProgressState::Paused;
    if (tbpf_flags & kTbpfIndeterminate) return ProgressState::Indeterminate;
    if (tbpf_flags & kTbpfNormal) return ProgressState::Normal;
    return ProgressState::NoProgress;
}

const char* ToString(ProgressState state) {
    switch (state) {
        case ProgressState::NoProgress: return "none";
        case ProgressState::Normal: return "normal";
        case ProgressState::Indeterminate: return "indeterminate";
        case ProgressState::Paused: return "paused";
        case ProgressState::Error: return "error";
    }
    return "?";
}

float ParseProgressPercent(const wchar_t* text) {
    if (!text) return -1.0f;
    const std::wstring s(text);
    // Anchor on the '%': explorer answers "56%", and a future source answering
    // "56% of 2 items" must yield 56, not the trailing 2. Without a '%', fall
    // back to the LAST digit run ("剩余 56" style) so a localized suffix still
    // beats a localized prefix.
    std::size_t start = std::wstring::npos;
    std::size_t end = std::wstring::npos;
    const std::size_t percent = s.find(L'%');
    if (percent != std::wstring::npos) {
        end = percent;
        while (end > 0 && !std::iswdigit(static_cast<wint_t>(s[end - 1]))) --end;
        if (end == 0) return -1.0f;
        start = end;
        while (start > 0 && std::iswdigit(static_cast<wint_t>(s[start - 1]))) --start;
    } else {
        end = s.size();
        while (end > 0 && !std::iswdigit(static_cast<wint_t>(s[end - 1]))) --end;
        if (end == 0) return -1.0f;
        start = end;
        while (start > 0 && std::iswdigit(static_cast<wint_t>(s[start - 1]))) --start;
    }
    if (start == end) return -1.0f;
    try {
        const long value = std::stol(s.substr(start, end - start));
        if (value < 0) return -1.0f;
        return std::min(1.0f, static_cast<float>(value) / 100.0f);
    } catch (...) {
        return -1.0f;
    }
}

// ---- CopyDialogMsaaReader ----------------------------------------------------

bool CopyDialogMsaaReader::IsWhiteListedClass(const wchar_t* class_name) {
    return IsWhiteListedClassImpl(class_name);
}

std::vector<ProgressReading> CopyDialogMsaaReader::ScanDialogs() {
    std::vector<ProgressReading> readings;
    // Re-initialising an already-initialised apartment is legal (S_FALSE) and
    // the matching CoUninitialize only decrements the count, so this is safe
    // whether or not the host initialised COM. RPC_E_CHANGED_MODE means the
    // thread already lives in the other apartment - proceed, MSAA works there.
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ScanContext ctx;
    EnumWindows(CollectWhiteListed, reinterpret_cast<LPARAM>(&ctx));
    for (HWND hwnd : ctx.matches) {
        if (!IsWindow(hwnd)) continue;  // destroyed mid-scan: drop, next poll
        WCHAR class_name[64] = L"";
        if (GetClassNameW(hwnd, class_name, 64) == 0) continue;
        ProgressReading reading;
        reading.hwnd = hwnd;
        reading.fraction = ReadProgressValue(hwnd, 0);
        reading.target_app_id = kProgressWhiteList[0].target_app_id;
        for (const ProgressWhiteListEntry& entry : kProgressWhiteList) {
            if (wcscmp(class_name, entry.window_class) == 0) {
                reading.target_app_id = entry.target_app_id;
                break;
            }
        }
        readings.push_back(reading);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return readings;
}

// ---- ProgressTracker ---------------------------------------------------------

ProgressTracker::ProgressTracker(int poll_interval_ms, ClockFn clock,
                                 IDialogProgressReader* reader)
    : poll_interval_ms_(poll_interval_ms),
      clock_(std::move(clock)),
      reader_(reader) {
    SetPollIntervalMs(poll_interval_ms_);
    if (!clock_) clock_ = [] { return NowMs(); };
}

ProgressTracker::~ProgressTracker() {
    if (own_reader_ && reader_) delete reader_;
}

void ProgressTracker::SetPollIntervalMs(int interval_ms) {
    poll_interval_ms_ = std::clamp(interval_ms, 250, 5000);
}

void ProgressTracker::SetReader(IDialogProgressReader* reader) {
    if (own_reader_ && reader_) delete reader_;
    reader_ = reader;
    own_reader_ = reader_ == nullptr;
    if (!reader_) {
        // Default constructed lazily here rather than in the constructor body:
        // the constructor's `reader` parameter already says "no injection",
        // and SetReader(nullptr) is the same statement.
        reader_ = new CopyDialogMsaaReader();
    }
}

bool ProgressTracker::Update(double delta_ms) {
    since_poll_ms_ += delta_ms;
    if (since_poll_ms_ < static_cast<double>(poll_interval_ms_)) return false;
    since_poll_ms_ = 0.0;
    if (!reader_) SetReader(nullptr);
    const double started = clock_();
    const bool changed = ScanNow();
    const double elapsed = clock_() - started;
    ++stats_.polls;
    stats_.last_poll_ms = elapsed;
    stats_.total_poll_ms += elapsed;
    stats_.max_poll_ms = std::max(stats_.max_poll_ms, elapsed);
    return changed;
}

bool ProgressTracker::ScanNow() {
    const std::vector<ProgressReading> readings = reader_->ScanDialogs();

    // Same-side cleanup (DoD): entries whose window left the scan are dropped
    // in this pass - mirrors ThumbnailHost::SetSources dropping slots whose
    // source is no longer wanted, on the same window-lifecycle side. The scan
    // is the source of truth: a window that still exists but is no longer
    // reported (MSAA failed mid-copy) is just as gone to the overlay.
    bool changed = false;
    for (auto it = windows_.begin(); it != windows_.end();) {
        const HWND hwnd = it->first;
        bool in_scan = false;
        for (const ProgressReading& r : readings) {
            if (r.hwnd == hwnd) in_scan = true;
        }
        if (!IsWindow(hwnd) || !in_scan) {
            it = windows_.erase(it);
            ++stats_.dropped;
            changed = true;
        } else {
            ++it;
        }
    }

    for (const ProgressReading& r : readings) {
        if (r.hwnd == nullptr) continue;
        auto it = windows_.find(r.hwnd);
        if (it == windows_.end()) {
            windows_[r.hwnd] = WindowState{r.fraction, r.target_app_id};
            ++stats_.readings;
            changed = true;
        } else if (it->second.fraction != r.fraction) {
            it->second.fraction = r.fraction;
            it->second.target_app_id = r.target_app_id;
            changed = true;
        }
    }

    // Collapse per-window state into the cached per-app answer. Keys are
    // lowercased so StateFor's ToLower(id) lookup always meets its match, no
    // matter how the white list spells the id.
    std::map<std::wstring, Snapshot> next;
    for (const auto& [hwnd, state] : windows_) {
        const std::wstring id = ToLower(
            std::wstring(state.target_app_id ? state.target_app_id : L""));
        Snapshot snap;
        snap.state = state.fraction < 0.0f ? ProgressState::Indeterminate : ProgressState::Normal;
        snap.fraction = state.fraction < 0.0f ? 0.0f : state.fraction;
        auto it = next.find(id);
        if (it == next.end()) {
            next.emplace(id, snap);
        } else if (snap.state == ProgressState::Normal &&
                   it->second.state == ProgressState::Normal &&
                   snap.fraction > it->second.fraction) {
            // Closest-to-done wins (see StateFor): the row the user waits on.
            it->second = snap;
        } else if (it->second.state != ProgressState::Normal) {
            it->second = snap;
        }
    }
    for (const auto& [id, snap] : next) {
        auto prev = last_.find(id);
        if (prev == last_.end() || prev->second.state != snap.state ||
            (snap.state == ProgressState::Normal &&
             std::fabs(prev->second.fraction - snap.fraction) > 0.0005f)) {
            if (changed_) changed_(id, snap.state, snap.fraction);
        }
    }
    // Apps that carried progress last poll and none now must fire a clearing
    // change - otherwise a finished copy leaves the overlay stuck on screen.
    for (const auto& [id, snap] : last_) {
        if (next.find(id) == next.end()) {
            if (changed_) changed_(id, ProgressState::NoProgress, 0.0f);
            changed = true;
        }
    }
    last_ = std::move(next);
    return changed;
}

ProgressTracker::Snapshot ProgressTracker::StateFor(const std::wstring& target_app_id) const {
    auto it = last_.find(ToLower(target_app_id));
    if (it == last_.end()) return Snapshot{};
    return it->second;
}

ProgressTracker::Snapshot ProgressTracker::StateForEntry(const AppEntry& app) const {
    if (!app.aumid.empty()) {
        const Snapshot snap = StateFor(app.aumid);
        if (snap.state != ProgressState::NoProgress) return snap;
    }
    if (!app.target_path.empty()) {
        return StateFor(BasenameLower(app.target_path));
    }
    return Snapshot{};
}

std::vector<std::wstring> ProgressTracker::ActiveTargetIds() const {
    std::vector<std::wstring> ids;
    ids.reserve(last_.size());
    for (const auto& [id, snap] : last_) {
        if (snap.state != ProgressState::NoProgress) ids.push_back(id);
    }
    return ids;
}

}  // namespace dockforge::shell
