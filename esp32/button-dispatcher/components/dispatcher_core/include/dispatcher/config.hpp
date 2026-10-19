// Config validation, serialization and import.
//
// The config is one JSON document (see README.md). It is validated whenever it is loaded, imported
// or edited through the API, so the rest of the firmware can rely on it being well-formed. The
// error messages are shown to users verbatim and the exported documents must import on the Pi
// implementation too, so the wording and the key order here are part of the contract.
//
// Nothing here throws (ESP-IDF builds without exceptions): failures are returned as values.
//
// Run test/run.sh (which builds test_config.cpp) every time this file or config.cpp is touched.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cJSON.h"
#include "dispatcher/models.hpp"

namespace dispatcher {

constexpr int kConfigVersion = 1;

// Owns a cJSON tree so that no early return can leak it.
using JsonPtr = std::unique_ptr<cJSON, void (*)(cJSON*)>;

// Wraps (and takes ownership of) a raw tree; nullptr gives an empty owner.
JsonPtr make_json(cJSON* raw = nullptr);

// A value or a user-facing error message.
template <typename T>
struct Parsed {
  std::optional<T> value;
  std::string error;
  bool ok() const { return value.has_value(); }
};

Parsed<Button> parse_button(const cJSON* json);
Parsed<Trigger> parse_trigger(const cJSON* json);
// Returns a normalized action: only the known fields are kept.
Parsed<Action> parse_action(const cJSON* json);
Parsed<Binding> parse_binding(const cJSON* json, const std::vector<std::string>& button_ids);
Parsed<Settings> parse_settings(const cJSON* json);

// Validates a whole config document. Bindings may also refer to `extra_button_ids`.
Parsed<Config> config_from_json(const cJSON* json, const std::vector<std::string>& extra_button_ids = {});
// Parses text first; text that is not JSON gives the error "invalid JSON".
Parsed<Config> config_from_text(const std::string& text);

// Returns `current` combined with an imported document. `mode` is "replace" (the document becomes
// the config) or "merge" (buttons and bindings with the same id are overwritten, others added).
// Stored SwitchBot credentials are kept unless the document carries its own.
Parsed<Config> import_config(const Config& current, const cJSON* data, const std::string& mode);

JsonPtr button_to_json(const Button& button);
JsonPtr binding_to_json(const Binding& binding);
JsonPtr action_to_json(const Action& action);
// Floating point settings are emitted as raw number text (for example "10.0"), so the tree is meant
// for printing, not for being read back with cJSON_GetNumberValue.
JsonPtr config_to_json(const Config& config, bool include_credentials);
// `pretty` uses two-space indentation and one entry per line; otherwise the text is compact.
std::string config_to_text(const Config& config, bool include_credentials, bool pretty);

// Returns "<prefix>-xxxxxx"; the six hex digits come from the low 24 bits of `random`.
std::string new_id(const std::string& prefix, uint32_t random);

}  // namespace dispatcher
