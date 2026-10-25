// DockForge - settings edit accumulation (T-050, CONTRACTS.md C-20).
//
// The pure half of the settings window: edits arrive from keyboard typing or from
// the probe channel, accumulate here, and flush onto a core::Config in one pass
// that clamps through the same range metadata the loader uses. Kept free of any
// HWND so the rules are unit-testable:
//
//   * an over-range value is applied CLAMPED, never rejected silently, and the
//     caller is told which fields moved (C-20: 回显钳制结果);
//   * edits to the same field replace each other, so typing "5", "56", "560"
//     inside one debounce window produces one flush with one value;
//   * unknown fields are rejected and reported, never dropped silently.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"

namespace dockforge::settings {

// One unapplied edit. `field` is the dotted C-01 key ("dock.icon_size"); `text`
// is the raw control content, parsed at flush time so an edit in progress never
// half-applies.
struct PendingEdit {
    std::string field;
    std::string text;
};

class SettingsModel {
  public:
    // Records (or replaces) the pending edit for `field`. Returns false when the
    // field is unknown, which is a caller bug rather than user error - the field
    // names come from this module's own control table.
    static bool KnowsField(const std::string& field);

    void Record(const std::string& field, const std::string& text);
    void Clear();
    bool HasPending() const { return !pending_.empty(); }
    std::size_t PendingCount() const { return pending_.size(); }

    // Applies every pending edit to `config`. Fields whose stored value moved
    // (a genuine edit OR a clamp landing on a different number) are appended to
    // `*changed` in field order, so the caller can echo the stored values back
    // into the controls (C-20). Unparsable values are appended to `*rejected`
    // and skipped; the stored config is untouched for them. Returns the number
    // of fields applied.
    int Apply(core::Config& config, std::vector<std::string>* changed = nullptr,
              std::vector<std::string>* rejected = nullptr) const;

    // Echoes `config`'s current values back into the pending-edit text form, so a
    // caller can refresh its controls after a clamp. Only fields the caller asks
    // about are produced.
    static std::string CurrentValueText(const core::Config& config,
                                        const std::string& field);

  private:
    std::vector<PendingEdit> pending_;  // insertion order; same-field replaces
};

}  // namespace dockforge::settings
