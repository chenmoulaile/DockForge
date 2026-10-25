// DockForge - per-app unread-message counters (T-065, CONTRACTS.md C-25).
//
// One adapter per target application, never a generic scheme (ADR-0022): the
// only system-wide source (UserNotificationListener) turned out to hold no
// persistent unread state for chat apps - the spike (spikes/T-065_notification_probe/)
// measured 51 notifications visible, none from WeChat or QQ, because chat apps
// do not keep unread messages in the notification centre. So the first real
// adapter reads what the target app itself exposes: its main-window TITLE
// ("(12) 微信" style), which ADR-0022 explicitly lists as the sanctioned
// observable surface. No memory hooks, no chat content, ever.
//
// The host owns registration, throttling and change notification so the dock
// main loop only ever sees "dirty or not". Every adapter failure degrades to
// count 0 - an adapter can never throw into the dock loop and never blocks it:
// the actual work is one CreateToolhelp32Snapshot + EnumWindows per second at
// most, the same scale the running indicator (T-012) already pays.
#pragma once

#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dockforge::shell {

// The C-25 adapter contract. Implementations must be self-contained: no global
// state, no exceptions across the boundary (catch inside CurrentCount).
class IMessageCounterAdapter {
  public:
    virtual ~IMessageCounterAdapter() = default;

    // AUMID or lowercase process name identifying the target. One id, one
    // adapter: the host refuses a second registration under the same id.
    virtual const std::wstring& TargetAppId() const = 0;

    // Returns false when the adapter cannot serve at all; the host then skips
    // it silently (one WARN, no retries, C-25).
    virtual bool Initialize() = 0;
    virtual void Shutdown() = 0;

    // Snapshot of the unread count. Must return 0 once the target is gone and
    // must never throw (C-25: failures answer 0).
    virtual int CurrentCount() = 0;
};

// Unread count parsed from a chat-app window title.
//
// "(12) 微信" -> 12, "WeChat (3)" -> 3, "微信" -> 0, "(0) 微信" -> 0.
// Scans every parenthesised group and takes the highest number: newer QQ/WeChat
// builds differ in which side they decorate, and a title like "(1) 搜狐新闻"
// inside a chat list must not be able to hide a larger badge elsewhere.
// Anything above 999 is clamped - a parse bug must not paint "1000+".
int ParseUnreadFromTitle(const std::wstring& title);

// The first real adapter: title-badge parsing for Win32 chat apps.
//
// Matches by process image name (lowercase, e.g. L"weixin.exe"), because the
// dock may hold an AUMID that differs from the process that owns the toast or
// the tray window. Candidates is a list so one adapter covers a family
// (weixin.exe / wechat.exe) without two registrations fighting over one tile.
class TitleBadgeAdapter : public IMessageCounterAdapter {
  public:
    TitleBadgeAdapter(std::wstring target_app_id, std::vector<std::wstring> process_names);

    const std::wstring& TargetAppId() const override { return target_app_id_; }
    bool Initialize() override;
    void Shutdown() override;
    int CurrentCount() override;

  private:
    std::wstring target_app_id_;
    std::vector<std::wstring> process_names_;  // lowercase, with .exe
};

// Owns the registered adapters (C-25). Not thread-safe: the dock loop is the
// only caller, like RunningTracker.
class MessageCounterHost {
  public:
    // Monotonic milliseconds. Injectable so tests drive the throttle without
    // sleeping; defaults to a steady-clock read.
    using ClockFn = std::function<double()>;

    // Fired only on changes, never on every poll. `count` is the new value.
    using CountChangedFn = std::function<void(const std::wstring& target_app_id, int count)>;

    explicit MessageCounterHost(int poll_interval_ms = 1000, ClockFn clock = nullptr);
    ~MessageCounterHost();

    MessageCounterHost(const MessageCounterHost&) = delete;
    MessageCounterHost& operator=(const MessageCounterHost&) = delete;

    // Registers an adapter. Takes ownership. When another adapter already owns
    // `TargetAppId()` the newcomer is dropped and a WARN is logged (C-25:
    // first registration wins). Returns false in that case.
    bool Register(std::unique_ptr<IMessageCounterAdapter> adapter);

    // Shuts down and removes the adapter for `target_app_id`, if present.
    // Returns true when one was removed.
    bool Unregister(const std::wstring& target_app_id);

    // Calls Initialize() on every registered adapter. Adapters answering false
    // are skipped with one WARN each and never retried (C-25). Idempotent.
    void InitializeAll();

    // Advances the throttle clock by querying each active adapter no more often
    // than poll_interval_ms. Fires CountChanged for every adapter whose count
    // moved since the last report (including to 0 when the target exits).
    // Returns true when any count changed, so the caller can mark the dock
    // dirty; a false return means the dock can stay asleep.
    bool Update();

    // Last reported count for an adapter id, or -1 when the id is unknown.
    int CountFor(const std::wstring& target_app_id) const;

    // Count for a dock tile's app. The tile is matched to an adapter by the
    // lowercase basename of its executable (Win32 entries) or by its AUMID
    // (packaged entries) - the same two identifier shapes C-25 names. A tile
    // with no adapter answers 0, which keeps the painter branch-free.
    int CountForEntry(const class AppEntry& app) const;

    std::size_t AdapterCount() const { return adapters_.size(); }
    bool IsEnabled(const std::wstring& target_app_id) const;  // settings gate

    // Per-adapter enable switches (C-01 integrations.message_counts.<id>.enabled).
    // A disabled adapter stops being polled and reports 0 through CountFor.
    void SetEnabled(const std::wstring& target_app_id, bool enabled);

    void set_count_changed(CountChangedFn handler) { count_changed_ = std::move(handler); }

    int poll_interval_ms() const { return poll_interval_ms_; }
    void SetPollIntervalMs(int interval_ms);

  private:
    struct Entry {
        std::unique_ptr<IMessageCounterAdapter> adapter;
        bool initialized = false;   // Initialize() answered true
        bool failed = false;        // Initialize() answered false: never retried
        int last_reported = -1;     // -1 until the first poll fires a change
        double last_poll_ms = -1e9;
    };

    Entry* Find(const std::wstring& target_app_id);

    int poll_interval_ms_;
    ClockFn clock_;
    CountChangedFn count_changed_;
    std::map<std::wstring, bool> enabled_;  // per adapter id, default true
    std::vector<Entry> adapters_;
};

}  // namespace dockforge::shell
