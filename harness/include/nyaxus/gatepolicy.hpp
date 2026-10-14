#pragma once

#include <string>

namespace nyaxus {
namespace gatepolicy {

// Structured permission decisions: every gate decision (manual approve /
// deny, readonly-argv auto-approve, always-allow bypass) is ledgered with a
// rule id, a reason, and the call's risk classification, so the operator
// can replay *why* a call ran from the event ledger.
//
// Risk taxonomy (deliberately coarse - the classifier is the source of
// truth for safety, the risk fields are for display and audit):
//   low      no state change (read-only manifest flag or argv classifier)
//   medium   workspace state (terminal writes, memory writes, web recording)
//   system   harness state (prompt.edit rewrites the active prompt pack)
// Side-effect scope: "none" | "workspace" | "system".

struct CallRisk {
    std::string risk_level;        // "low" | "medium" | "system"
    std::string side_effect_scope; // "none" | "workspace" | "system"
};

// Classify a tool call. `read_only_call` is the daemon's verdict for this
// exact call (manifest read_only flag, or the argv classifier proving the
// terminal command changes no state). `posix_shell` is false for the
// cmd.exe fallback: a write there is not provably scoped to the workspace,
// so it classifies as system.
CallRisk classify(const std::string& tool, const std::string& args_json,
                  bool read_only_call, bool posix_shell);

}  // namespace gatepolicy
}  // namespace nyaxus
