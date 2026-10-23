// DockForge - type-safe synchronous event bus (CONTRACTS.md C-06).
//
// Design constraints:
//  - Publish is synchronous and allocation-free in the steady state.
//  - Subscribing or unsubscribing from inside a handler is legal. Unsubscribe
//    marks the entry dead immediately, so a handler removed mid-dispatch does
//    not fire even if it was already scheduled; the vector is compacted after
//    the outermost dispatch returns.
//  - Handlers added during dispatch do not receive the event in flight.
//  - No RTTI-heavy dispatch: one vector per event type, keyed by type_index.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dockforge::core {

class EventBus {
  public:
    using SubscriptionId = std::uint64_t;

    EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    template <typename Event, typename Handler>
    SubscriptionId Subscribe(Handler&& handler) {
        auto& channel = channels_[std::type_index(typeid(Event))];
        // Reserve on first use so a hot publish path never reallocates.
        if (channel.capacity() == 0) channel.reserve(8);
        const SubscriptionId id = next_id_++;
        channel.push_back(
            Entry{id, true,
                  [fn = std::forward<Handler>(handler)](const void* payload) {
                      fn(*static_cast<const Event*>(payload));
                  }});
        return id;
    }

    void Unsubscribe(SubscriptionId id) {
        if (id == 0) return;
        for (auto& [type, channel] : channels_) {
            for (auto& entry : channel) {
                if (entry.id != id) continue;
                entry.alive = false;
                if (dispatch_depth_ == 0) {
                    Compact(channel);
                } else {
                    pending_removal_ = true;
                }
                return;
            }
        }
    }

    template <typename Event>
    void Publish(const Event& event) {
        const auto it = channels_.find(std::type_index(typeid(Event)));
        if (it == channels_.end()) return;

        auto& channel = it->second;
        ++dispatch_depth_;

        // Index-based loop: handlers may append to `channel` during dispatch.
        // `delivered` pins the boundary so late subscribers miss this event.
        const std::size_t delivered = channel.size();
        for (std::size_t i = 0; i < delivered; ++i) {
            if (!channel[i].alive) continue;
            // Copy the callable: the handler itself may unsubscribe (which can
            // compact the vector) while we are still inside it.
            const auto callback = channel[i].fn;
            callback(&event);
        }

        --dispatch_depth_;
        if (dispatch_depth_ == 0 && pending_removal_) {
            pending_removal_ = false;
            for (auto& [type, list] : channels_) Compact(list);
        }
    }

    // Number of live handlers for a type; used by tests and diagnostics.
    template <typename Event>
    std::size_t HandlerCount() const {
        const auto it = channels_.find(std::type_index(typeid(Event)));
        if (it == channels_.end()) return 0;
        std::size_t live = 0;
        for (const auto& entry : it->second) {
            if (entry.alive) ++live;
        }
        return live;
    }

    std::size_t TotalHandlerCount() const {
        std::size_t live = 0;
        for (const auto& [type, list] : channels_) {
            for (const auto& entry : list) {
                if (entry.alive) ++live;
            }
        }
        return live;
    }

    // Drops every handler. Used on shutdown so no callback can fire afterwards.
    void Clear() {
        channels_.clear();
        pending_removal_ = false;
        dispatch_depth_ = 0;
    }

  private:
    struct Entry {
        SubscriptionId id;
        bool alive;
        std::function<void(const void*)> fn;
    };

    static void Compact(std::vector<Entry>& channel) {
        channel.erase(std::remove_if(channel.begin(), channel.end(),
                                     [](const Entry& e) { return !e.alive; }),
                      channel.end());
    }

    std::unordered_map<std::type_index, std::vector<Entry>> channels_;
    SubscriptionId next_id_ = 1;
    int dispatch_depth_ = 0;
    bool pending_removal_ = false;
};

}  // namespace dockforge::core
