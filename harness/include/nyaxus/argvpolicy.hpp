#pragma once

#include <string>
#include <vector>

namespace nyaxus {
namespace argvpolicy {

// Design: docs/readonly-argv.md. Fail-closed classifier for the terminal
// tool: true only when the whole command line is provably read-only in the
// shell the daemon is about to run it in (a unix /bin/sh - git-bash on
// Windows, /bin/sh on POSIX - or cmd.exe as the last resort). Anything the
// scanner cannot prove safe - unknown commands, dynamic expansions,
// redirects to a real file, unproven git subcommands - returns false with a
// reason, and the call falls through to the approval gate.

// The shell that will execute the command. The daemon picks it (unix shell
// preferred, cmd fallback) and passes it in, so the classifier proves
// read-onlyness against exactly the shell that runs the call.
enum class Shell { Posix, Cmd };

struct Verdict {
    bool read_only = false;
    std::string reason;  // gate reason (empty when read_only)
};

Verdict evaluate(const std::string& command, Shell shell);

// The shell parse used by evaluate() and the fingerprint rulebook: the line
// split into simple commands on top-level `;` `&&` `||` `|` and newlines,
// words quote/escape-aware. `ok` is false (with a `reason`) on any dynamic
// syntax - subshells, backgrounding, unmatched quotes, expansions, dangling
// operators - so a failed parse means "not pattern-able", never "empty".
// Flavor-independent: the scanner is the conservative union of both shells.
struct SimpleCommand {
    std::vector<std::string> words;
    std::vector<bool> quoted;  // per word: contained a quote character
};
struct ParsedCommand {
    bool ok = false;
    std::string reason;
    std::vector<SimpleCommand> commands;
};
ParsedCommand parse(const std::string& command);

}  // namespace argvpolicy
}  // namespace nyaxus
