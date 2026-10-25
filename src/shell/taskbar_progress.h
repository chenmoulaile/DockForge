// DockForge - white-list taskbar progress reading (T-067, ADR-0024, C-22).
//
// ADR-0024 requires the read path to be proven before any product code. The
// spike (spikes/T-067_progress_probe/) measured every candidate:
//
//   * ITaskbarList3 window-property proxy state: SetProgressValue and
//     SetProgressState both return S_OK and leave ZERO properties on the target
//     window - the state is not stored where another process could EnumProps it
//     back, so the preferred read path is dead on arrival (evidence:
//     spikes/T-067_progress_probe/self_run.txt).
//   * vtable hook: not probed - ADR-0024 rules it the same class of forbidden
//     target-process memory modification as ADR-0022.
//   * MSAA walk of the explorer copy dialog: WORKS cross-process. The dialog's
//     window class is "OperationStatusWindow" (one word; its title is EMPTY, so
//     title matching never fires), and its DirectUI tree answers
//     get_accValue() = L"56%" on a ROLE_SYSTEM_PROGRESSBAR element (evidence:
//     spikes/T-067_progress_probe/watch_run3.txt).
//
// So the shipped reader is the ADR-0024 degrade path: white-list polling. One
// source ships (explorer copy); Chrome downloads remain an open white-list
// entry. The read is strictly passive - MSAA queries only, nothing is ever
// written into or injected toward a target process (ADR-0024 forbidden list).
//
// Lifecycle discipline mirrors ThumbnailHost: state lives per HWND, and every
// poll rebuilds the active set from the scan - a window that vanished from the
// scan (or fails IsWindow) is dropped in the same pass, the same side of the
// window lifecycle where ThumbnailHost::SetSources calls DwmUnregisterThumbnail
// on slots whose source left the wanted list. No background thread; the MSAA
// interfaces are released before every return, so no handle survives a poll.
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "shell/app_entry.h"

namespace dockforge::shell {

// The DoD state machine, one value per TBPF_* flag group. Normal advances,
// Indeterminate pulses, Paused holds at #FFB900, Error at #E81123 (C-22).
enum class ProgressState {
    NoProgress,     // TBPF_NOPROGRESS (0): nothing is painted
    Normal,         // TBPF_NORMAL
    Indeterminate,  // TBPF_INDETERMINATE
    Paused,         // TBPF_PAUSED
    Error,          // TBPF_ERROR
};

// Maps raw TBPF_* flag bits to the state machine. Explorer never exposes the
// flags to readers (they are write-only, see the spike), so this exists for the
// mapping the DoD names and for injected test sources. Flags are documented as
// mutually exclusive, but a caller that ORs several gets the most severe state
// rather than an arbitrary one: Error > Paused > Indeterminate > Normal.
ProgressState ProgressStateFromFlags(int tbpf_flags);

const char* ToString(ProgressState state);

// Parses a progress-bar accessible value. Explorer's copy dialog answers
// L"56%" (also seen: L"0%"). Returns the 0..1 fraction, or -1.0f when the text
// carries no percentage. Clamped both ways: a buggy "105%" must not push the
// bar past its slot.
float ParseProgressPercent(const wchar_t* text);

// One white-list entry: dialogs of `window_class` credit `target_app_id`.
// The id is matched against a tile's AUMID (lowercase) or the lowercase
// basename of its executable - the two identifier shapes C-25 names. Matching
// is by class, NOT by owning process: the copy dialog can live in a COM
// surrogate (measured: a fresh pid every copy), while the progress still
// belongs to the File Explorer tile in the user's mental model.
//
// The explorer id is the AUMID, not "explorer.exe": the dock's File Explorer
// tile resolves as a ShellItem whose aumid is Microsoft.Windows.Explorer and
// whose target_path is EMPTY (measured, see the pin-kept log line in the
// T-067 probe evidence) - an exe-name id would never match it.
struct ProgressWhiteListEntry {
    const wchar_t* window_class;
    const wchar_t* target_app_id;
};

// The shipped white list. Explorer copy only for now; Chrome downloads are the
// named-but-not-yet-covered entry (ADR-0024 allows "white-list works" as the
// bar, not full-system coverage).
inline constexpr ProgressWhiteListEntry kProgressWhiteList[] = {
    {L"OperationStatusWindow", L"Microsoft.Windows.Explorer"},
};

// One dialog's reading. fraction is 0..1; -1.0f means "no percentage came
// back" and maps to Indeterminate: the dialog exists and is busy, but there is
// no number to draw.
struct ProgressReading {
    HWND hwnd = nullptr;
    float fraction = -1.0f;
    const wchar_t* target_app_id = L"";  // borrowed from the white list
};

// Injectable scan interface so the tracker's state machine is unit-testable
// without a live copy dialog (same pattern as IMessageCounterAdapter, C-25).
class IDialogProgressReader {
  public:
    virtual ~IDialogProgressReader() = default;

    // Every white-listed dialog with its current reading. Called on the
    // tracker's poll clock only - never per frame.
    virtual std::vector<ProgressReading> ScanDialogs() = 0;
};

// The shipped reader: explorer copy dialogs via MSAA.
//
// Scan cost: one EnumWindows plus, per matching dialog, an accessible-object
// walk of a small DirectUI tree (single-digit ms, spike-measured). At the 500ms
// default poll that is noise; SetPollIntervalMs' clamp keeps a bad override
// from turning it into a busy loop.
class CopyDialogMsaaReader : public IDialogProgressReader {
  public:
    std::vector<ProgressReading> ScanDialogs() override;

  private:
    // Window-class allow-list check, separated so a test can exercise it.
    static bool IsWhiteListedClass(const wchar_t* class_name);
};

// Polls the reader, keeps per-window state, answers per-app snapshots.
//
// Not thread-safe: the dock loop is the only caller, like RunningTracker.
class ProgressTracker {
  public:
    // Injectable clock so tests drive the throttle without sleeping.
    using ClockFn = std::function<double()>;

    explicit ProgressTracker(int poll_interval_ms = 500, ClockFn clock = nullptr,
                             IDialogProgressReader* reader = nullptr);
    ~ProgressTracker();

    ProgressTracker(const ProgressTracker&) = delete;
    ProgressTracker& operator=(const ProgressTracker&) = delete;

    // Clamped to 250..5000: an MSAA sweep costs real milliseconds, so the
    // floor sits far above RunningTracker's 50ms (that one buys a
    // sub-microsecond foreground read); the ceiling keeps a running copy from
    // freezing the overlay.
    void SetPollIntervalMs(int interval_ms);
    int poll_interval_ms() const { return poll_interval_ms_; }

    // Swaps the reader (never owned; null restores the built-in
    // CopyDialogMsaaReader). Test seam - production leaves it alone.
    void SetReader(IDialogProgressReader* reader);

    // Advances the poll clock; scans when due. Returns true when the visible
    // state may have changed (a reading appeared, moved, or a window dropped
    // out) so the caller repaints; false means the dock can stay asleep.
    bool Update(double delta_ms);

    // Forces the next Update to scan immediately, so the first frame after a
    // copy started does not show an empty overlay for one interval.
    void Invalidate() { since_poll_ms_ = 1e9; }

    struct Snapshot {
        ProgressState state = ProgressState::NoProgress;
        float fraction = 0.0f;  // 0..1, meaningful for Normal only
    };

    // Snapshot for one white-listed app id (lowercase basename or AUMID).
    // Multiple live dialogs for the same id answer the one closest to done -
    // the row the user is most likely waiting on (explorer normally merges
    // copies into one dialog anyway; this covers the multi-window tail).
    Snapshot StateFor(const std::wstring& target_app_id) const;

    // Snapshot for a dock tile's app - same matching as
    // MessageCounterHost::CountForEntry: AUMID first, then lowercase basename
    // of the executable. NoProgress for apps outside the white list.
    Snapshot StateForEntry(const AppEntry& app) const;

    // White-listed app ids that currently carry progress (log/debug).
    std::vector<std::wstring> ActiveTargetIds() const;

    // Same disclosure habit as RunningTracker::Stats.
    struct Stats {
        std::uint64_t polls = 0;
        std::uint64_t readings = 0;  // dialogs found, summed over polls
        std::uint64_t dropped = 0;   // windows removed (destroyed / left scan)
        double last_poll_ms = 0.0;
        double total_poll_ms = 0.0;
        double max_poll_ms = 0.0;
    };
    const Stats& GetStats() const { return stats_; }

    // Fired on every state change (app id, state, fraction). Logging only -
    // the dock repaints off Update's return value, never from this callback.
    using ChangedFn = std::function<void(const std::wstring&, ProgressState, float)>;
    void set_changed(ChangedFn handler) { changed_ = std::move(handler); }

  private:
    struct WindowState {
        float fraction = -1.0f;
        const wchar_t* target_app_id = L"";
    };

    bool ScanNow();

    int poll_interval_ms_;
    ClockFn clock_;
    IDialogProgressReader* reader_ = nullptr;
    bool own_reader_ = false;  // true while using the built-in default
    ChangedFn changed_;

    // Milliseconds since the last scan. Starts "due" (1e9) so the first
    // Update scans immediately - a copy already running when the dock starts
    // must show its bar on the first painted frame, not one interval later.
    double since_poll_ms_ = 1e9;
    // HWND -> last reading. Rebuilt against every scan: entries that left the
    // scan are erased in the same pass (the DwmUnregisterThumbnail-same-side
    // cleanup the DoD demands - see file header).
    std::map<HWND, WindowState> windows_;
    // Cached per-app answer so StateFor is stable between polls.
    std::map<std::wstring, Snapshot> last_;
    Stats stats_{};
};

}  // namespace dockforge::shell
