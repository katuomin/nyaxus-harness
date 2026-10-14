#pragma once

#include <string>
#include <vector>

namespace nyaxus {
namespace fingerprint {

// Design: docs/fingerprint-rulebook.md. Argument-fingerprint patterns for
// the tiered always-allow ("always" scope on the approval card). A pattern
// scopes the allow to a shape of call instead of the whole tool:
//
//   terminal  -> one pattern per simple command of the parsed
//                sequence/pipeline (every subcommand must match later):
//                  "git status"   ^cmd\s+sub   (subcommand tools)
//                  "^/a/b/"       ^parent/     (path-scoped tools)
//                  "ls"           ^cmd         (whole command)
//   web.fetch -> the URL host ("example.com")
//
// A rule with an empty pattern list means "the whole tool": the call was not
// pattern-able (dynamic expansion, parse failure, path-like command name,
// cmd.exe shell) and the operator's "always" trusts the tool itself.
struct Rule {
    std::string tool;
    std::vector<std::string> patterns;
    std::string label;  // human-readable, for card tooltip and settings list
};

// Derive the rule for an approved call. `args_json` is the tool's raw
// argument JSON (the daemon's tool_arguments string). `posix_shell` is
// false for the cmd.exe fallback: without POSIX chaining, command patterns
// are not expressible, so terminal degrades to whole-tool.
Rule derive(const std::string& tool, const std::string& args_json,
            bool posix_shell);

// True when the call matches the stored rule: same tool, and for terminal
// the command parses to the same number of simple commands, each matching
// its pattern; for web.fetch the host matches. `patterns` is the stored
// serialized patterns ("" = whole tool).
bool matches(const std::string& tool, const std::string& patterns,
             const std::string& args_json);

// Storage round-trip for the pattern list (JSON array; "" = whole tool).
std::string serialize_patterns(const std::vector<std::string>& patterns);
std::vector<std::string> parse_patterns(const std::string& serialized);

// Human-readable label for a stored serialized pattern ("" = whole tool),
// for the approvals endpoint and the settings list.
std::string label_for(const std::string& serialized_patterns);

}  // namespace fingerprint
}  // namespace nyaxus
