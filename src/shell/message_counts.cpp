// DockForge - per-app unread-message counters (T-065, CONTRACTS.md C-25).
#include "shell/message_counts.h"

#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cwctype>

#include "core/log.h"
#include "shell/app_entry.h"

namespace dockforge::shell {
namespace {

// Lowercases in place (ASCII + basic wide; identity strings are ASCII or CJK
// where case never matters).
std::wstring ToLower(std::wstring text) {
    for (wchar_t& ch : text) ch = static_cast<wchar_t>(std::towlower(ch));
    return text;
}

// Basename of a path, e.g. "D:\Weixin\Weixin.exe" -> "weixin.exe".
std::wstring BasenameLower(const std::wstring& path) {
    const std::wstring lower = ToLower(path);
    const std::size_t slash = lower.find_last_of(L"/\\");
    return slash == std::wstring::npos ? lower : lower.substr(slash + 1);
}

}  // namespace

int ParseUnreadFromTitle(const std::wstring& title) {
    // Every "(digits)" group in the title is a candidate; the highest wins.
    // Both observed layouts - "(12) 微信" prefix and "WeChat (12)" suffix - and
    // hypothetical "(3) (5) 项目" multi-group titles are covered by the same
    // scan, so no per-app tweak can rot silently when a client changes layout.
    int best = 0;
    std::size_t i = 0;
    while (i < title.size()) {
        if (title[i] != L'(') {
            ++i;
            continue;
        }
        const std::size_t open = i;
        std::size_t j = open + 1;
        int value = 0;
        bool digits = false;
        while (j < title.size() && title[j] >= L'0' && title[j] <= L'9') {
            value = value * 10 + (title[j] - L'0');
            digits = true;
            ++j;
        }
        if (digits && j < title.size() && title[j] == L')' && value > best) {
            best = value;
        }
        i = (j > open + 1) ? j : open + 1;
    }
    // A parse bug must not paint "1000+": real unread badges are small.
    if (best > 999) best = 999;
    return best;
}

// ---- TitleBadgeAdapter ------------------------------------------------------

TitleBadgeAdapter::TitleBadgeAdapter(std::wstring target_app_id,
                                     std::vector<std::wstring> process_names)
    : target_app_id_(std::move(target_app_id)), process_names_(std::move(process_names)) {}

bool TitleBadgeAdapter::Initialize() {
    // Stateless: every CurrentCount() is a fresh snapshot. Nothing to probe
    // here - a wrong process name simply answers 0, which is the degradation
    // the contract asks for, not an initialization failure.
    return true;
}

void TitleBadgeAdapter::Shutdown() {}

int TitleBadgeAdapter::CurrentCount() {
    // Whole scan in a try/catch: C-25 forbids exceptions crossing into the dock
    // loop. The work is one process snapshot + one window enumeration, the same
    // order of cost the running indicator pays every 300ms.
    int result = 0;
    try {
        // pid -> lowercase image basename, built once per call. EnumWindows
        // alone cannot answer "which exe owns this window" without an
        // OpenProcess per window (a handle to close on every branch); the
        // snapshot is one pass over processes and then a map lookup.
        std::map<DWORD, std::wstring> names;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    names[entry.th32ProcessID] = ToLower(entry.szExeFile);
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }

        struct Scan {
            const std::vector<std::wstring>* names;
            const std::map<DWORD, std::wstring>* pids;
            int best;
        } ctx{&process_names_, &names, 0};

        EnumWindows(
            [](HWND hwnd, LPARAM lparam) -> BOOL {
                auto* scan = reinterpret_cast<Scan*>(lparam);
                if (!IsWindowVisible(hwnd)) return TRUE;
                // Title-less windows cannot carry a badge, skip before the
                // (comparatively costly) pid lookup.
                wchar_t title[256];
                const int len = GetWindowTextW(hwnd, title, 256);
                if (len <= 0) return TRUE;

                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                const auto it = scan->pids->find(pid);
                if (it == scan->pids->end()) return TRUE;
                bool owned = false;
                for (const std::wstring& name : *scan->names) {
                    if (it->second == name) {
                        owned = true;
                        break;
                    }
                }
                if (!owned) return TRUE;
                const int unread = ParseUnreadFromTitle(title);
                if (unread > scan->best) scan->best = unread;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&ctx));

        result = ctx.best;
    } catch (...) {
        result = 0;  // C-25: failures answer 0.
    }
    return result;
}

// ---- MessageCounterHost -----------------------------------------------------

MessageCounterHost::MessageCounterHost(int poll_interval_ms, ClockFn clock)
    : poll_interval_ms_(poll_interval_ms),
      clock_(clock ? std::move(clock)
                   : [] {
                         return std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                             .count();
                     }) {}

MessageCounterHost::~MessageCounterHost() {
    for (Entry& entry : adapters_) {
        if (entry.initialized) entry.adapter->Shutdown();
    }
}

void MessageCounterHost::SetPollIntervalMs(int interval_ms) {
    // Same clamp philosophy as RunningTracker: a bad value must not turn the
    // dock into a busy loop (min 200ms) or freeze the badge (max 10s).
    poll_interval_ms_ = std::min(std::max(interval_ms, 200), 10000);
}

MessageCounterHost::Entry* MessageCounterHost::Find(const std::wstring& target_app_id) {
    for (Entry& entry : adapters_) {
        if (entry.adapter->TargetAppId() == target_app_id) return &entry;
    }
    return nullptr;
}

bool MessageCounterHost::Register(std::unique_ptr<IMessageCounterAdapter> adapter) {
    if (!adapter) return false;
    const std::wstring id = adapter->TargetAppId();
    if (Find(id) != nullptr) {
        // C-25: one adapter per TargetAppId, first registration wins.
        DF_WARN("shell", "message-count adapter for '%s' already registered, keeping the first",
                ToLower(id).c_str());
        return false;
    }
    // Defaults to enabled; the settings gate (integrations.message_counts) can
    // turn it off before InitializeAll is called.
    enabled_.try_emplace(ToLower(id), true);
    adapters_.push_back(Entry{std::move(adapter), false, false, -1, -1e9});
    return true;
}

bool MessageCounterHost::Unregister(const std::wstring& target_app_id) {
    for (auto it = adapters_.begin(); it != adapters_.end(); ++it) {
        if (it->adapter->TargetAppId() == target_app_id) {
            if (it->initialized) it->adapter->Shutdown();
            adapters_.erase(it);
            return true;
        }
    }
    return false;
}

void MessageCounterHost::InitializeAll() {
    for (Entry& entry : adapters_) {
        if (entry.failed || entry.initialized) continue;
        const bool enabled = IsEnabled(entry.adapter->TargetAppId());
        if (!enabled) continue;  // stays uninitialized until switched on
        entry.initialized = entry.adapter->Initialize();
        if (!entry.initialized) {
            // C-25: one WARN, no retries. A permanently broken adapter must not
            // cost the dock loop anything after this point.
            entry.failed = true;
            DF_WARN("shell", "message-count adapter '%s' unavailable, skipped",
                    ToLower(entry.adapter->TargetAppId()).c_str());
        }
    }
}

bool MessageCounterHost::IsEnabled(const std::wstring& target_app_id) const {
    const auto it = enabled_.find(ToLower(target_app_id));
    return it == enabled_.end() ? true : it->second;
}

void MessageCounterHost::SetEnabled(const std::wstring& target_app_id, bool enabled) {
    enabled_[ToLower(target_app_id)] = enabled;
    if (Entry* entry = Find(target_app_id); entry != nullptr) {
        if (!enabled && entry->initialized) {
            // Turning a switch off must also stop the last count from showing:
            // answer 0 from here on, exactly like the target having exited.
            entry->adapter->Shutdown();
            entry->initialized = false;
            entry->last_reported = 0;
            entry->last_poll_ms = -1e9;
            if (count_changed_) count_changed_(entry->adapter->TargetAppId(), 0);
        }
        if (entry->failed && enabled) entry->failed = false;  // one retry per re-enable
    }
}

bool MessageCounterHost::Update() {
    const double now = clock_ ? clock_() : 0.0;
    bool changed = false;
    for (Entry& entry : adapters_) {
        if (!entry.initialized) {
            // Re-check the settings gate: an adapter enabled after startup gets
            // its Initialize here instead of a separate code path.
            if (IsEnabled(entry.adapter->TargetAppId()) && !entry.failed) {
                entry.initialized = entry.adapter->Initialize();
                if (!entry.initialized) {
                    entry.failed = true;
                    DF_WARN("shell", "message-count adapter '%s' unavailable, skipped",
                            ToLower(entry.adapter->TargetAppId()).c_str());
                }
            }
            continue;
        }
        if (!IsEnabled(entry.adapter->TargetAppId())) continue;
        if (now - entry.last_poll_ms < static_cast<double>(poll_interval_ms_)) continue;
        entry.last_poll_ms = now;

        int count = 0;
        try {
            count = entry.adapter->CurrentCount();
        } catch (...) {
            count = 0;  // C-25: adapter failures answer 0, never propagate.
        }
        if (count != entry.last_reported) {
            entry.last_reported = count;
            changed = true;
            if (count_changed_) count_changed_(entry.adapter->TargetAppId(), count);
        }
    }
    return changed;
}

int MessageCounterHost::CountFor(const std::wstring& target_app_id) const {
    for (const Entry& entry : adapters_) {
        if (entry.adapter->TargetAppId() == target_app_id) {
            // Disabled or uninitialized adapters answer 0, mirroring "target
            // not running" - the badge simply has nothing to say.
            return (entry.initialized && IsEnabled(entry.adapter->TargetAppId()))
                       ? std::max(entry.last_reported, 0)
                       : 0;
        }
    }
    return -1;
}

int MessageCounterHost::CountForEntry(const AppEntry& app) const {
    // C-25 names two identifier shapes - AUMID or process name - and a tile can
    // carry either: Start-menu ShellItems expose an AUMID, .lnk-resolved entries
    // only a path. Try both, first positive wins, 0 when neither matches.
    if (!app.aumid.empty()) {
        const int count = CountFor(ToLower(app.aumid));
        if (count > 0) return count;
    }
    if (!app.target_path.empty()) {
        const int count = CountFor(BasenameLower(app.target_path));
        if (count > 0) return count;
    }
    return 0;
}

}  // namespace dockforge::shell
