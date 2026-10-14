#pragma once

#include <string>

#include "nyaxus/argvpolicy.hpp"

namespace nyaxus {
namespace denyguard {

// Design: docs/decisions/deny-guards.md. A guard layer in front of the
// approval waterfall: it can only deny, never allow. Because it has no allow
// verdict, no bypass path is even expressible against it - an always-allow
// fingerprint, a session scope, or the readonly-argv classifier can pre-
//approve a call, but none of them can pre-approve a guard denial.
//
// Two scan families, both deliberately conservative (a miss is a gate, a
// false positive is a visible denial with a reason):
//   - secret-argument scan: credential-shaped values in any tool's
//     arguments (known key names, provider key prefixes, PEM headers,
//     JWTs). A secret in a tool argument ends up in the provider request
//     and the ledger; the guard keeps it out of ungated paths.
//   - obviously-destructive argv: the terminal commands that destroy
//     state a recovery cannot (root/home rm -rf, mkfs, dd to a block
//     device, power cycle, force push).

struct Verdict {
    bool deny = false;
    std::string reason;  // human-readable; ledgered and fed back to the model
};

// `tool` is the tool name, `arguments` its raw argument JSON, `shell` the
// shell the terminal tool will run in (irrelevant for non-terminal tools).
Verdict evaluate(const std::string& tool,
                 const std::string& arguments,
                 argvpolicy::Shell shell);

}  // namespace denyguard
}  // namespace nyaxus
