// DockForge - notification types and sidecar line parsing (T-076).
#include "notifications/notification_types.h"

#include "core/paths.h"

#include <nlohmann/json.hpp>

namespace dockforge::notifications {
namespace {

// Longest a single field may be before it is cut. The sidecar already clamps,
// but a hand-written probe line can carry anything and a row must never be
// laid out from an unbounded string.
constexpr std::size_t kMaxFieldChars = 600;
constexpr std::size_t kMaxItems = 64;

std::wstring Trim(std::wstring s) {
    // Only the three characters the sidecar can actually emit around a field.
    // Note the polarity: this predicate answers "is this whitespace", and the
    // loops below strip WHILE it holds - inverting it here silently eats the
    // whole string, which is how a field can arrive non-empty and render blank.
    const auto is_space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r'; };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    return s;
}

std::wstring Clamped(const std::string& utf8) {
    std::wstring wide = core::Utf8ToWide(utf8);
    if (wide.size() > kMaxFieldChars) wide.resize(kMaxFieldChars);
    return Trim(std::move(wide));
}

}  // namespace

const char* ToString(AccessStatus status) {
    switch (status) {
        case AccessStatus::Unspecified:
            return "unspecified";
        case AccessStatus::Allowed:
            return "allowed";
        case AccessStatus::Denied:
            return "denied";
    }
    return "unknown";
}

std::wstring NotificationItem::DisplayApp() const {
    if (!app.empty()) return app;
    if (!aumid.empty()) return aumid;
    return L"通知";
}

std::wstring NotificationItem::Headline() const {
    if (!title.empty()) return title;
    if (!body.empty()) {
        const std::size_t nl = body.find(L'\n');
        return nl == std::wstring::npos ? body : body.substr(0, nl);
    }
    return DisplayApp();
}

std::wstring NotificationItem::Detail() const {
    if (title.empty()) {
        // The headline was already taken from the body's first line, so the
        // rest of the body is the detail.
        const std::size_t nl = body.find(L'\n');
        if (nl == std::wstring::npos) return {};
        return Trim(body.substr(nl + 1));
    }
    return Trim(body);
}

bool ParseNotificationLine(const std::string& line, NotificationState* out, bool* is_push,
                           bool* ready, std::wstring* error) {
    if (out != nullptr) *out = NotificationState{};
    if (is_push != nullptr) *is_push = false;
    if (ready != nullptr) *ready = false;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (...) {
        return false;
    }
    if (!j.is_object()) return false;

    if (j.contains("push") && j["push"].is_string()) {
        if (is_push != nullptr) *is_push = true;
        return true;
    }
    if (!j.contains("ok") || !j["ok"].is_boolean()) return false;
    if (!j["ok"].get<bool>()) {
        if (error != nullptr) {
            std::string raw;
            if (j.contains("error") && j["error"].is_string()) {
                raw = j["error"].get<std::string>();
            }
            *error = core::Utf8ToWide(raw);
        }
        return true;
    }

    // Ready banner: carries the access answer but no notification list.
    if (j.contains("sidecar") && j["sidecar"].is_string()) {
        if (ready != nullptr) *ready = true;
        if (out != nullptr && j.contains("access") && j["access"].is_number_integer()) {
            out->access = static_cast<AccessStatus>(j["access"].get<int>());
        }
        return true;
    }

    if (out == nullptr) return true;
    if (!j.contains("items") || !j["items"].is_array()) {
        // A bare ack ({"ok":true}) is a valid line with no state.
        return true;
    }
    if (j.contains("access") && j["access"].is_number_integer()) {
        out->access = static_cast<AccessStatus>(j["access"].get<int>());
    }
    out->total = j.value("count", 0);
    for (const auto& node : j["items"]) {
        if (!node.is_object()) continue;
        NotificationItem item;
        item.id = node.value("id", 0u);
        item.aumid = Clamped(node.value("aumid", std::string()));
        item.app = Clamped(node.value("app", std::string()));
        item.title = Clamped(node.value("title", std::string()));
        item.body = Clamped(node.value("body", std::string()));
        item.created_unix = node.value("created", static_cast<std::int64_t>(0));
        out->items.push_back(std::move(item));
        if (out->items.size() >= kMaxItems) break;
    }
    out->valid = true;
    return true;
}

}  // namespace dockforge::notifications
