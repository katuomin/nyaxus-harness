#include "nyaxus/argvpolicy.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nyaxus {
namespace argvpolicy {
namespace {

constexpr std::size_t kMaxCommandLength = 10000;

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// Command names are case-insensitive in cmd.exe and case-sensitive in
// /bin/sh; the safe tables are stored lowercase.
std::string normalize_name(const std::string& name, Shell shell) {
    return shell == Shell::Cmd ? to_lower(name) : name;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// The null device per shell: `NUL` in cmd.exe, `/dev/null` in /bin/sh
// (accepting `NUL` in sh would let `> NUL` create a file named NUL).
bool is_nul_target(const std::string& target, Shell shell) {
    const std::string t = to_lower(target);
    if (shell == Shell::Cmd) return t == "nul" || t == "nul:";
    return t == "/dev/null";
}

// ── Safe tables ─────────────────────────────────────────────────────────
// Env-assignment names allowed as a POSIX prefix (`NAME=value cmd`):
// presentation/locale/runtime toggles that cannot by themselves change state.
const std::set<std::string>& safe_env_names() {
    static const std::set<std::string> names = {
        "ANTHROPIC_API_KEY", "BLOCK_SIZE",     "BLOCKSIZE",
        "CGO_ENABLED",       "CHARSET",        "CI",
        "CLICOLOR",          "CLICOLOR_FORCE", "COLORTERM",
        "COLUMNS",           "DEBIAN_FRONTEND", "FORCE_COLOR",
        "GCC_COLORS",        "GIT_TERMINAL_PROMPT", "GO111MODULE",
        "GOARCH",            "GOEXPERIMENT",   "GOOS",
        "GREP_COLOR",        "GREP_COLORS",    "LANG",
        "LANGUAGE",          "LC_ALL",         "LC_CTYPE",
        "LC_TIME",           "LINES",          "LSCOLORS",
        "LS_COLORS",         "NO_COLOR",       "NODE_ENV",
        "PYTEST_DEBUG",      "PYTEST_DISABLE_PLUGIN_AUTOLOAD",
        "PYTHONDONTWRITEBYTECODE", "PYTHONUNBUFFERED", "RUST_BACKTRACE",
        "RUST_LOG",          "TERM",           "TIME_STYLE",
        "TZ",
    };
    return names;
}

// Commands that take no arguments at all.
const std::set<std::string>& command_only_names(Shell shell) {
    static const std::set<std::string> posix = {
        "pwd", "true", "false", "whoami", "hostname", "ver",
    };
    static const std::set<std::string> cmd = {
        "ver", "whoami", "hostname",
    };
    return shell == Shell::Cmd ? cmd : posix;
}

// Commands safe with any arguments (no flag can change their read-only
// nature; output redirects are handled by the scanner, and `date -s`-style
// writers are simply not in the set).
const std::set<std::string>& any_args_names(Shell shell) {
    static const std::set<std::string> posix = {
        "ls", "cat", "du", "df", "file", "which", "wc", "sort",
        "uniq", "cut", "tr", "basename", "dirname", "realpath",
        "readlink", "stat", "strings", "hexdump", "od", "nl", "head",
        "tail",
        "seq", "sleep", "uname", "ps", "pgrep", "man", "echo",
        "printf", "grep", "egrep", "fgrep", "rg", "fd", "fdfind",
        "jq", "test", "[",
    };
    static const std::set<std::string> cmd = {
        "dir", "type", "findstr", "where", "echo", "cd",
        "pushd", "popd", "tree", "netstat", "tasklist",
        "systeminfo", "vol",
    };
    return shell == Shell::Cmd ? cmd : posix;
}

// Commands whose safety depends on their flags: any argument word that
// equals a deny flag (or starts with it, so `-i.bak` matches `-i`) gates.
struct FlagPolicy {
    bool command_only = false;
    std::vector<std::string> deny_flags;
};

const std::map<std::string, FlagPolicy>& flag_policies(Shell shell) {
    static const std::map<std::string, FlagPolicy> posix = {
        // sed writes only in place; -i (any suffix form) and --in-place gate.
        {"sed", {false, {"-i", "--in-place"}}},
        // find's write actions are the -delete/-exec/-ok/-fprint family.
        {"find", {false, {"-delete", "-exec", "-execdir", "-ok", "-okdir",
                          "-fprint", "-fprintf", "-fls", "-printf"}}},
    };
    static const std::map<std::string, FlagPolicy> cmd = {
        // icacls prints the ACL by default; the grant/remove/owner switches
        // change it.
        {"icacls", {false, {"/grant", "/remove", "/setowner", "/reset"}}},
        // cmd `set` with no arguments prints the environment; `set X=Y`,
        // `set /p` (stdin read) and `set /a` (arithmetic) gate.
        {"set", {true, {}}},
    };
    return shell == Shell::Cmd ? cmd : posix;
}

// git: read-only subcommands safe with any arguments.
const std::set<std::string>& git_read_only_any_args() {
    static const std::set<std::string> subs = {
        "status", "log", "show", "shortlog", "whatchanged", "version",
        "help", "rev-list", "rev-parse", "describe", "ls-files",
        "count-objects", "verify-pack", "for-each-ref", "cat-file",
        "grep", "blame", "fsck",
    };
    return subs;
}

// git: subcommands that write repository state (the reason string names
// them for the ledger).
const std::set<std::string>& git_known_write() {
    static const std::set<std::string> subs = {
        "add", "am", "archive", "bisect", "bundle", "cherry-pick", "clean",
        "clone", "commit", "fetch", "gc", "init", "maintenance", "merge",
        "mv", "pack-refs", "prune", "pull", "push", "rebase", "reset",
        "restore", "revert", "rm", "svn", "switch", "update-ref",
        "write-tree",
    };
    return subs;
}

// ── Git policy ──────────────────────────────────────────────────────────
// Global flags that can point git at a different repository or write
// config. `--no-pager`/`--paginate` are the only allowed globals.
bool git_global_flag_ok(const std::string& flag) {
    return flag == "--no-pager" || flag == "--paginate";
}

int count_positional(const std::vector<std::string>& args) {
    int count = 0;
    for (const std::string& arg : args) {
        if (!arg.empty() && arg[0] != '-') ++count;
    }
    return count;
}

Verdict evaluate_git(const std::vector<std::string>& args, Shell shell) {
    std::size_t i = 0;
    while (i < args.size() && !args[i].empty() && args[i][0] == '-') {
        if (!git_global_flag_ok(args[i])) {
            return {false, "git global flag: " + args[i]};
        }
        ++i;
    }
    if (i >= args.size()) {
        return {false, "git with no subcommand"};
    }
    const std::string sub = normalize_name(args[i], shell);
    std::vector<std::string> rest(args.begin() + i + 1, args.end());
    if (sub == "diff") {
        for (const std::string& word : rest) {
            if (word == "-o" || starts_with(word, "--output")) {
                return {false, "git diff --output writes a file"};
            }
        }
        return {true, ""};
    }
    if (git_read_only_any_args().count(sub)) {
        return {true, ""};
    }
    // List mode: no positional arguments (a positional would create,
    // delete, or select state: `git tag v1`, `git branch new`, ...).
    if (sub == "tag" || sub == "branch" || sub == "remote") {
        if (count_positional(rest) > 0) {
            return {false, "git " + sub + " with a positional argument "
                           "changes state"};
        }
        return {true, ""};
    }
    // `git config key` reads; `git config key value` writes.
    if (sub == "config") {
        if (count_positional(rest) > 1) {
            return {false, "git config with two keys writes"};
        }
        return {true, ""};
    }
    if (sub == "index-version") {
        if (count_positional(rest) > 0) {
            return {false, "git index-version with an argument upgrades"};
        }
        return {true, ""};
    }
    if (sub == "symbolic-ref") {
        if (count_positional(rest) != 1) {
            return {false, "git symbolic-ref needs exactly one argument"};
        }
        return {true, ""};
    }
    // Fixed read-only verbs.
    if (sub == "reflog" || sub == "stash" || sub == "worktree") {
        const std::string verb = (sub == "reflog") ? "show" : "list";
        if (rest.empty() || normalize_name(rest[0], shell) != verb) {
            return {false, "git " + sub + " only in " + verb + " mode"};
        }
        return {true, ""};
    }
    if (git_known_write().count(sub)) {
        return {false, "git " + sub + " writes state"};
    }
    return {false, "unknown git subcommand: " + sub};
}

// ── Per-command evaluation ──────────────────────────────────────────────
Verdict evaluate_command(const std::vector<std::string>& words,
                         const std::vector<bool>& quoted, Shell shell) {
    std::size_t first = 0;
    // Env-assignment prefix (`NAME=value cmd`) is a /bin/sh construct:
    // allowed only for safe presentation/locale names. In cmd.exe the same
    // syntax is not an assignment, so the word becomes the (unknown) command
    // name and gates naturally.
    if (shell == Shell::Posix) {
        while (first < words.size()) {
            const std::string& word = words[first];
            const std::size_t eq = word.find('=');
            const bool looks_like_assignment =
                eq != std::string::npos && eq > 0 && !quoted[first] &&
                std::isalpha(static_cast<unsigned char>(word[0]));
            if (!looks_like_assignment) break;
            if (!safe_env_names().count(word.substr(0, eq))) {
                return {false, "env assignment not in the safe list: " +
                                   word.substr(0, eq)};
            }
            ++first;
        }
    }
    if (first >= words.size()) {
        return {false, "no command"};
    }
    const std::string name = normalize_name(words[first], shell);
    std::vector<std::string> rest(words.begin() + first + 1, words.end());
    if (name == "git") {
        return evaluate_git(rest, shell);
    }
    if (command_only_names(shell).count(name)) {
        if (!rest.empty()) {
            return {false, name + " takes no arguments"};
        }
        return {true, ""};
    }
    const auto it = flag_policies(shell).find(name);
    if (it != flag_policies(shell).end()) {
        if (it->second.command_only) {
            if (!rest.empty()) {
                return {false, name + " with arguments gates"};
            }
            return {true, ""};
        }
        for (const std::string& word : rest) {
            const std::string normalized = normalize_name(word, shell);
            for (const std::string& deny : it->second.deny_flags) {
                if (starts_with(normalized, to_lower(deny))) {
                    return {false, name + " write flag: " + word};
                }
            }
        }
        return {true, ""};
    }
    if (any_args_names(shell).count(name)) {
        return {true, ""};
    }
    return {false, "unknown command: " + name};
}

// ── Scanner ─────────────────────────────────────────────────────────────
// Splits the line into simple commands on top-level `;`, `&&`, `||`, `|`,
// and newlines, tracking quotes/escapes and validating redirects. The
// scanner is deliberately the *union* of both shells' dynamic syntax:
// anything that looks dynamic in cmd.exe *or* /bin/sh gates, so one
// conservative pass works for the platform the daemon compiles for.
struct ParseResult {
    bool ok = false;
    std::string reason;
    std::vector<std::vector<std::string>> commands;
    std::vector<std::vector<bool>> quoted;  // per word: contained a quote
};

// Reads one word starting at `i` (skipping leading spaces). Returns the
// index just past the word, or -1 if the word ran into an operator. Sets
// `quoted` when the word contained any quote character.
std::size_t read_word(const std::string& s, std::size_t i,
                      std::string& word, bool& quoted) {
    word.clear();
    quoted = false;
    const std::size_t n = s.size();
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (i >= n) return i;
    char quote = 0;
    while (i < n) {
        const char c = s[i];
        if (quote == 0) {
            if (c == ' ' || c == '\t' || c == ';' || c == '&' || c == '|' ||
                c == '<' || c == '>' || c == '\n' || c == '\r') {
                return i;
            }
            if (c == '(' || c == ')') {
                return static_cast<std::size_t>(-1);  // subshell
            }
            if (c == '$' || c == '`' || c == '%' || c == '~' || c == '!') {
                return static_cast<std::size_t>(-1);  // dynamic expansion
            }
            if (c == '"' || c == '\'') {
                quote = c;
                quoted = true;
                ++i;
                continue;
            }
            if (c == '^') {
                // cmd escape: the next character is literal.
                ++i;
                if (i < n) {
                    word += s[i];
                    ++i;
                }
                continue;
            }
            if (c == '\\') {
                // POSIX escape: the next character is literal (on cmd the
                // backslash is a path separator; dropping it here can only
                // make a command name *less* recognizable, i.e. gate).
                ++i;
                if (i < n) {
                    word += s[i];
                    ++i;
                }
                continue;
            }
            word += c;
            ++i;
            continue;
        }
        // Quoted: everything is literal except the escape rules.
        if (c == '\\' && quote == '"') {
            ++i;
            if (i < n) {
                word += s[i];
                ++i;
            }
            continue;
        }
        if (c == quote) {
            quote = 0;
            ++i;
            continue;
        }
        if (c == '$' || c == '`' || c == '%' || c == '~' || c == '!') {
            return static_cast<std::size_t>(-1);  // expansion inside "..."
        }
        word += c;
        ++i;
    }
    if (quote != 0) return static_cast<std::size_t>(-1);  // unterminated
    return i;
}

ParseResult parse_commands(const std::string& s, Shell shell) {
    ParseResult result;
    std::vector<std::vector<std::string>> commands;
    std::vector<std::vector<bool>> quoted;
    std::vector<std::string> words;
    std::vector<bool> word_quoted;

    auto end_command = [&]() {
        if (!words.empty()) {
            commands.push_back(std::move(words));
            quoted.push_back(std::move(word_quoted));
            words.clear();
            word_quoted.clear();
        }
    };

    const std::size_t n = s.size();
    std::size_t i = 0;
    bool pending_op = false;  // a &&/||/| is waiting for its right side
    while (i < n) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '\n' || c == ';') {
            end_command();
            pending_op = false;
            ++i;
            continue;
        }
        if (c == '&') {
            if (i + 1 < n && s[i + 1] == '&') {
                if (words.empty() && commands.empty()) {
                    return ParseResult{false, "dangling operator", {},
                                       {}};
                }
                end_command();
                pending_op = true;
                i += 2;
                continue;
            }
            if (i + 1 < n && s[i + 1] == '>') {
                // `&>` falls through to the redirect parser below.
            } else {
                return ParseResult{false, "backgrounding (&)", {}, {}};
            }
        }
        if (c == '|') {
            if (i + 1 < n && s[i + 1] == '|') {
                if (words.empty() && commands.empty()) {
                    return ParseResult{false, "dangling operator", {},
                                       {}};
                }
                end_command();
                pending_op = true;
                i += 2;
                continue;
            }
            if (words.empty() && commands.empty()) {
                return ParseResult{false, "dangling operator", {}, {}};
            }
            end_command();
            pending_op = true;
            ++i;
            continue;
        }
        if (c == '<' || c == '>' || c == '&') {
            // Redirect / fd-duplication. The word just ended may be a
            // single digit (the file descriptor: `2>`, `1>&2`).
            int fd = -1;
            if (!words.empty() && words.back().size() == 1 &&
                std::isdigit(static_cast<unsigned char>(words.back()[0]))) {
                fd = words.back()[0] - '0';
                words.pop_back();
                word_quoted.pop_back();
            }
            std::string op;
            std::size_t j = i;
            while (j < n && (s[j] == '<' || s[j] == '>' || s[j] == '&')) {
                op += s[j];
                ++j;
            }
            while (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) {
                op += s[j];
                ++j;
            }
            const bool dup = (op == ">&1" || op == ">&2" ||
                              (op.size() == 3 && op[1] == '&' &&
                               std::isdigit(static_cast<unsigned char>(op[2])))) ||
                             (op.size() == 3 && op[0] == '<' && op[1] == '&' &&
                              std::isdigit(static_cast<unsigned char>(op[2])));
            if (op == "<") {
                // Input redirect: reads a file, no state change.
                i = j;
                continue;
            }
            if (dup) {
                i = j;
                continue;
            }
            if (op == ">" || op == ">>" || op == "&>" ||
                (fd >= 0 && (op == ">" || op == ">>"))) {
                // Output redirect: allowed only to NUL (the daemon's own
                // `2>&1` is a dup, handled above).
                std::string target;
                bool target_quoted = false;
                const std::size_t end = read_word(s, j, target, target_quoted);
                if (end == static_cast<std::size_t>(-1) ||
                    !is_nul_target(target, shell)) {
                    return ParseResult{false,
                                       "output redirect to a file: " + target,
                                       {}, {}};
                }
                i = end;
                continue;
            }
            return ParseResult{false, "redirect: " + op, {}, {}};
        }
        // A word.
        std::string word;
        bool word_q = false;
        const std::size_t end = read_word(s, i, word, word_q);
        if (end == static_cast<std::size_t>(-1)) {
            return ParseResult{false, "unsupported syntax", {}, {}};
        }
        words.push_back(std::move(word));
        word_quoted.push_back(word_q);
        pending_op = false;
        i = end;
    }
    end_command();
    if (commands.empty()) {
        return ParseResult{false, "no command", {}, {}};
    }
    if (pending_op) {
        return ParseResult{false, "dangling operator", {}, {}};
    }
    return ParseResult{true, "", std::move(commands), std::move(quoted)};
}

}  // namespace

Verdict evaluate(const std::string& command, Shell shell) {
    std::string trimmed = command;
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() &&
           std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        return {false, "empty command"};
    }
    if (trimmed.size() > kMaxCommandLength) {
        return {false, "command too long"};
    }
    const ParseResult parsed = parse_commands(trimmed, shell);
    if (!parsed.ok) {
        return {false, parsed.reason};
    }
    for (std::size_t index = 0; index < parsed.commands.size(); ++index) {
        const Verdict verdict =
            evaluate_command(parsed.commands[index], parsed.quoted[index],
                             shell);
        if (!verdict.read_only) {
            return verdict;
        }
    }
    return {true, ""};
}

ParsedCommand parse(const std::string& command) {
    ParsedCommand result;
    // The scanner is flavor-independent except for the NUL-redirect target
    // check; Posix is the conservative choice for a public parse (a `NUL`
    // target under Posix fails the check, i.e. the parse reports no-command-
    // shape - fail-closed, and such lines gate in evaluate() too).
    const ParseResult parsed = parse_commands(command, Shell::Posix);
    if (!parsed.ok) {
        result.reason = parsed.reason;
        return result;
    }
    result.ok = true;
    result.commands.reserve(parsed.commands.size());
    for (std::size_t index = 0; index < parsed.commands.size(); ++index) {
        SimpleCommand simple;
        simple.words = parsed.commands[index];
        simple.quoted = parsed.quoted[index];
        result.commands.push_back(std::move(simple));
    }
    return result;
}

}  // namespace argvpolicy
}  // namespace nyaxus
