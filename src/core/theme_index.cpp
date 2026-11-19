// DockForge - community theme index (T-085, ADR-0026).
#include "core/theme_index.h"

#include "core/log.h"
#include "platform/http_client.h"

#include <cctype>
#include <cstdint>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace dockforge::core {
namespace {

using json = nlohmann::json;

constexpr char kLogModule[] = "theme_index";
constexpr std::size_t kMaxIndexBytes = 4 * 1024 * 1024;  // the index is text; 4MB is generous

}  // namespace

bool IsValidThemeName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok) return false;
    }
    if (name == "default" || name == "dark") return false;  // built-ins (C-02/T-052)
    if (name.find("..") != std::string::npos) return false;  // traversal, belt and braces
    return true;
}

bool ParseThemeIndex(const std::string& json_text, std::vector<ThemeIndexEntry>& out,
                     std::string* error) {
    out.clear();
    auto fail = [&](const char* reason) {
        if (error != nullptr) *error = reason;
        DF_WARN(kLogModule, "index rejected: %s", reason);
        return false;
    };
    if (json_text.size() > kMaxIndexBytes) return fail("index larger than 4MB");

    const json parsed = json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return fail("malformed JSON");
    if (parsed.contains("schema") && !parsed["schema"].is_number_integer()) {
        return fail("schema is not an integer");
    }
    if (parsed.contains("schema") && parsed["schema"].get<int>() != 1) {
        return fail("unsupported schema");
    }
    const auto themes = parsed.find("themes");
    if (themes == parsed.end() || !themes->is_array()) return fail("missing themes array");

    for (const auto& node : *themes) {
        if (!node.is_object()) return fail("entry is not an object");
        ThemeIndexEntry entry;
        const auto text_of = [&](const char* key) -> std::string {
            const auto it = node.find(key);
            if (it == node.end() || !it->is_string()) return {};
            return it->get<std::string>();
        };
        entry.name = text_of("name");
        entry.author = text_of("author");
        entry.version = text_of("version");
        entry.preview_url = text_of("preview_url");
        entry.download_url = text_of("download_url");
        entry.sha256 = text_of("sha256");

        if (node.contains("size_bytes") && node["size_bytes"].is_number_unsigned()) {
            entry.size_bytes = static_cast<long long>(node["size_bytes"].get<std::uint64_t>());
        }

        // Per-entry validation: one bad entry rejects the whole document (the
        // installer never sees a partially validated row).
        if (!IsValidThemeName(entry.name)) return fail("bad theme name");
        if (entry.download_url.rfind("http://", 0) != 0 &&
            entry.download_url.rfind("https://", 0) != 0) {
            return fail("download_url must be http(s)");
        }
        if (!entry.preview_url.empty() && entry.preview_url.rfind("http://", 0) != 0 &&
            entry.preview_url.rfind("https://", 0) != 0) {
            return fail("preview_url must be http(s) or empty");
        }
        if (entry.sha256.size() != 64) return fail("sha256 must be 64 hex chars");
        for (char& c : entry.sha256) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!(std::isdigit(static_cast<unsigned char>(c)) ||
                  (c >= 'a' && c <= 'f'))) {
                return fail("sha256 must be hex");
            }
        }
        if (entry.author.size() > 128) return fail("author longer than 128 bytes");
        if (entry.version.size() > 64) return fail("version longer than 64 bytes");
        if (entry.size_bytes < 0) return fail("negative size_bytes");

        // Duplicate names would make the browser ambiguous about what a click
        // installs; reject rather than guess.
        for (const ThemeIndexEntry& existing : out) {
            if (existing.name == entry.name) return fail("duplicate theme name");
        }
        out.push_back(std::move(entry));
    }
    return true;
}

bool FetchThemeIndex(const std::string& url_utf8, std::vector<ThemeIndexEntry>& out,
                     std::string* error) {
    const platform::HttpBytesResult fetched = platform::HttpGetBytes(url_utf8, 15000);
    if (!fetched.ok) {
        if (error != nullptr) {
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "index fetch failed (status=%lu err=%lu)",
                          fetched.status, fetched.error);
            *error = buffer;
        }
        DF_WARN(kLogModule, "fetch %s failed status=%lu err=%lu", url_utf8.c_str(),
                fetched.status, fetched.error);
        return false;
    }
    return ParseThemeIndex(fetched.body, out, error);
}

}  // namespace dockforge::core
