// DockForge - theme pack loading (T-052, CONTRACTS.md C-02).
//
// A theme pack is themes/<name>/theme.json:
//   { "name", "author", "version", "extends", "overrides": { <C-01 subset> } }
//
// Rules the loader enforces (all unit-tested):
//   * overrides may only set keys the contract marks themeable - unknown keys
//     are WARNed and ignored, never applied, and never dropped from the file;
//   * `extends` chains base-first (base overrides first, pack overrides last),
//     with a depth limit so a circular extends chain cannot hang the dock;
//   * the two built-in themes ("default", "dark") resolve even when the themes
//     directory is missing entirely;
//   * any failure to resolve a theme leaves the config untouched - the caller
//     falls back and the dock keeps its current look (never black).
//
// v1 honesty note: C-01 v1 froze no colour keys, so a theme can only differ in
// the themeable metrics (material, corner_radius, dock/launchpad sizes). A dark
// theme in the visual sense needs a colour surface in the schema, which is the
// v1.1 gap recorded in the roadmap tasks.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"

namespace dockforge::core {

struct ThemeInfo {
    std::string name;
    std::string author;
    std::string version;
    std::string extends;  // empty = no base
};

// True when `key` (dotted C-01 name) may be set by a theme pack.
bool IsThemeableKey(const std::string& key);

// Applies one overrides object to `config`. Unknown or non-themeable keys are
// appended to `*ignored` and skipped; values that fail to parse are also
// ignored. Returns the number of keys applied.
int ApplyThemeOverrides(const std::string& overrides_json, core::Config& config,
                        std::vector<std::string>* ignored = nullptr);

// Resolves theme `name` and applies its overrides (extends chain first) onto
// `config`. "default" applies nothing. Returns false when the theme (or its
// base) cannot be read; `config` is untouched in that case.
bool LoadThemeInto(const std::string& name, core::Config& config, std::string* error);

// Theme ids available right now: the two built-ins plus every directory under
// ThemesDir() that holds a parseable theme.json. Built-ins always come first.
std::vector<std::string> ListThemes();

}  // namespace dockforge::core
