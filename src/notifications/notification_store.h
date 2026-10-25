// DockForge - in-memory notification history (T-076, ADR-0025).
//
// ADR-0025 is the reason this class has no Load/Save: notification content is
// the most sensitive data the app touches, and persisting it would immediately
// require answers about encryption, cleanup, backup and migration. The history
// panel is therefore empty after a restart - by design, not by omission.
//
// The store also owns the "what is new" decision, because that is the only part
// with a subtle rule: the FIRST successful poll of a session is a baseline, not
// an event. Windows keeps a backlog of toasts, so treating the first answer as
// arrivals would greet every launch with a burst of banners for messages the
// user read hours ago (measured: 62 notifications on this machine).
#pragma once

#include "notifications/notification_types.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace dockforge::notifications {

// How many rows the history panel keeps. The sidecar caps its own answer at 40;
// this is the local ceiling so a chatty app cannot grow the list without bound.
inline constexpr std::size_t kMaxHistoryItems = 40;

class NotificationStore {
  public:
    // Replaces the visible list with `state.items` (newest first) and reports
    // what arrived since the last call through `arrived`.
    //
    //  * First accepted state: seeds the id set, `arrived` stays empty.
    //  * Later states: every id not seen before is an arrival, oldest first, so
    //    a burst of three banners appears in the order Windows recorded them.
    //  * An item the listener no longer holds is dropped from the history too -
    //    the panel shows what the system currently has, not a stale copy.
    //
    // Returns true when anything visible changed (the caller marks the dock
    // dirty). `arrived` is never filled for an invalid state.
    bool ApplyState(const NotificationState& state, std::vector<NotificationItem>* arrived);

    // Local clear: empties the list and remembers the ids as "already seen" so
    // the next poll does not re-announce everything the user just dismissed.
    void ClearLocal();

    // Forgets one item (a banner was dismissed, or the user removed the row).
    // The id stays in the seen set for the same reason as ClearLocal.
    void Forget(std::uint32_t id);

    const std::vector<NotificationItem>& items() const { return items_; }
    std::size_t size() const { return items_.size(); }
    bool empty() const { return items_.empty(); }
    // True once a poll answer has been accepted; before that the panel must say
    // "waiting" rather than "no notifications".
    bool primed() const { return primed_; }
    int total_reported() const { return total_reported_; }
    AccessStatus access() const { return access_; }
    void set_access(AccessStatus status) { access_ = status; }

    // Index of the item with `id`, or -1.
    int IndexOf(std::uint32_t id) const;

  private:
    static bool SameList(const std::vector<NotificationItem>& a,
                         const std::vector<NotificationItem>& b);

    std::vector<NotificationItem> items_;
    std::set<std::uint32_t> seen_;
    bool primed_ = false;
    int total_reported_ = 0;
    AccessStatus access_ = AccessStatus::Unspecified;
};

}  // namespace dockforge::notifications
