#pragma once

// ConfigParser — reads a .bench file into a HubConfig (ConfigTypes.h).
// Constraints:
//   - Line-oriented, no indentation sensitivity: a block keyword (hub/state/
//     intent/event/stream/store) opens a new block; every following line
//     belongs to it until the next block keyword. `#` starts a comment.
//   - Deliberately NOT YAML: see README.md's "why not YAML" note. The whole
//     grammar is documented there; do not extend it here without updating
//     that reference first.
//   - Never throws; failure is reported through the return value + `error`.
// See: sim/slopbench/README.md (config format reference, grammar table).

#include <string>

#include "config/ConfigTypes.h"

namespace slopbench {

// Parses `path` into `out`. Returns true on success; on failure returns false
// and leaves a human-readable reason in `error` (line number + what's wrong).
// Runs the cross-reference validation pass too (INTENT `writes` targets
// resolve, field keys are unique/ascending per channel, no duplicate channel
// ids) — a config that parses but fails validation still returns false.
bool loadConfigFile(const std::string& path, HubConfig& out, std::string& error);

}  // namespace slopbench
