// DockForge - in-memory notification history (see notification_store.h).
#include "notifications/notification_store.h"

#include <algorithm>

namespace dockforge::notifications {

bool NotificationStore::ApplyState(const NotificationState& state,
                                   std::vector<NotificationItem>* arrived) {
    if (arrived != nullptr) arrived->clear();
    if (!state.valid) return false;
    access_ = state.access;
    total_reported_ = state.total;

    // The listener hands back newest first. Walk backwards so arrivals are
    // reported oldest-first - three toasts from one app then stack in the order
    // they were raised instead of upside down.
    std::vector<NotificationItem> fresh;
    for (auto it = state.items.rbegin(); it != state.items.rend(); ++it) {
        if (seen_.insert(it->id).second) fresh.push_back(*it);
    }

    const bool was_primed = primed_;
    primed_ = true;

    std::vector<NotificationItem> next;
    next.reserve(std::min(state.items.size(), kMaxHistoryItems));
    for (const NotificationItem& item : state.items) {
        if (next.size() >= kMaxHistoryItems) break;
        next.push_back(item);
    }
    const bool list_changed = !SameList(items_, next);
    items_ = std::move(next);

    if (!was_primed) {
        // Baseline: the backlog Windows already held is history, not news.
        return list_changed;
    }
    if (arrived != nullptr && !fresh.empty()) *arrived = std::move(fresh);
    return list_changed || (arrived != nullptr && !arrived->empty());
}

bool NotificationStore::SameList(const std::vector<NotificationItem>& a,
                                 const std::vector<NotificationItem>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].id != b[i].id) return false;
    }
    return true;
}

void NotificationStore::ClearLocal() {
    // Keep the ids in `seen_`: a clear is a local dismissal, and the next poll
    // still returns the same toasts from Windows. Forgetting them would make
    // every cleared item pop straight back up as an arrival.
    items_.clear();
}

void NotificationStore::Forget(std::uint32_t id) {
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [id](const NotificationItem& item) { return item.id == id; }),
                 items_.end());
}

int NotificationStore::IndexOf(std::uint32_t id) const {
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace dockforge::notifications
