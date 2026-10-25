// DockForge - optional secret storage for keyed providers (T-075).
//
// Weather providers that need a key (AMap) read it from
// %APPDATA%\DockForge\secrets.json instead of config.json, so a config that
// is shared, screenshotted or committed to a dotfiles repo never carries a
// credential. The file is user data: it is never written by the app, never
// logged, and never lands in the repository. DOCKFORGE_SECRETS_PATH mirrors
// DOCKFORGE_CONFIG_PATH for tests and probes (same isolation contract,
// CONTRACTS.md C-03 neighborhood).
#pragma once

#include <map>
#include <string>

namespace dockforge::core {

// Path of the secrets file: %APPDATA%\DockForge\secrets.json, or the
// DOCKFORGE_SECRETS_PATH override when set.
std::wstring SecretsPath();

// Parses {"<key>": "<value>", ...}. Missing file or malformed JSON returns an
// empty map - a missing key must degrade to "provider without key", never to
// a crash or a config reset. Values are never logged by this module.
std::map<std::string, std::string> ReadSecrets();

}  // namespace dockforge::core
