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

// A word hits a deny flag when it starts with it (bundled suffixes: `-i`
// matches `-i.bak`), or, for a single-letter short option, when the word
// is a short-option cluster containing that letter (`-ni` matches an `-i`
// deny; `grep -nR` matches `-R`). Long options are prefix matches only.
// Deny flags compare case-sensitively: `-R` and `-r` are different GNU
// grep options (one follows symlinks, one does not), and the cmd tables
// already list their switches lowercase.
bool flag_hit(const std::string& word, const std::string& deny) {
    if (starts_with(word, deny)) return true;
    if (deny.size() == 2 && deny[0] == '-' && word.size() >= 3 &&
        word[0] == '-' && word[1] != '-') {
        return word.find(deny[1], 1) != std::string::npos;
    }
    return false;
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
// writers are simply not in the set). Commands with process-spawn or
// shell-pager options (man, fd/fdfind) live in the flag-policy table
// below, not here.
const std::set<std::string>& any_args_names(Shell shell) {
    static const std::set<std::string> posix = {
        "ls", "cat", "du", "df", "file", "which", "wc",
        "uniq", "cut", "tr", "basename", "dirname", "realpath",
        "readlink", "stat", "strings", "hexdump", "od", "nl", "head",
        "tail",
        "seq", "sleep", "uname", "ps", "pgrep", "echo",
        "printf",
        "jq", "test", "[",
    };
    static const std::set<std::string> cmd = {
        "dir", "type", "findstr", "where", "echo", "cd",
        "pushd", "popd", "tree", "netstat", "tasklist",
        "systeminfo", "vol",
    };
    return shell == Shell::Cmd ? cmd : posix;
}

// Commands whose safety depends on their flags. The write-flag scan is
// getopt-aware: short bundles are walked letter by letter, data options
// consume their value argument, and `--` ends the options where the
// command guarantees end-of-options semantics (see scan_arguments).
struct FlagPolicy {
    bool command_only = false;
    std::vector<std::string> deny_flags;
    // Data options: flags whose argument the command itself interprets as
    // data - pattern text, script text, sort specs, replacement text - not
    // a filesystem operand and never an option. The option and the argument
    // it consumes are exempt from the deny scan, so command data that
    // merely looks like a write flag stop gating (`grep -e -Rx` searches
    // for the string "-Rx", `sort -k -o` sorts by the key "-o"). git
    // read-only subcommands already grant their pathspec arguments this
    // status wholesale (any args); the flag-policy commands get it per
    // option. Entries of exactly two characters also cover getopt bundles
    // (`-ne` = -n + -e <next word>: an argument-taking letter eats the rest
    // of the bundle, or the next word when it is last).
    std::vector<std::string> data_flags;
    // The command honors `--` (end of options): everything after it is a
    // bare operand/data word and is exempt (`grep -- -R f` searches for the
    // string "-R"). Enabled only where the end-of-options semantics are
    // guaranteed (GNU grep/sed/sort, ripgrep/clap); `find` has no `--` and
    // stays fail-closed without it.
    bool allow_end_of_options = false;
};

const std::map<std::string, FlagPolicy>& flag_policies(Shell shell) {
    static const std::map<std::string, FlagPolicy> posix = {
        // sed: the script (-e/--expression, -f/--file) is command data.
        // Writes gate through -i/--in-place AND the inline-script scan
        // (evaluate_sed): the script itself can write files or run a
        // shell via w/W/r/R/e and the s-command w/e flags, and -f/--file
        // loads a script from an unvouched file.
        {"sed", {false, {"-i", "--in-place"},
                 {"-e", "--expression", "-f", "--file"}, true}},
        // find: the -name/-path/-regex-style predicates take pattern data;
        // GNU find consumes the next argv element unconditionally, even if
        // it looks like an option (verified on GNU findutils: `-name
        // -delete`/`-name -L` is a pattern, never the action/traversal
        // option). Write actions and the -H/-L traversal options stay
        // denied, everywhere the scan can attribute them.
        {"find", {false, {"-delete", "-exec", "-execdir", "-ok",
                          "-okdir", "-fprint", "-fprintf", "-fls",
                          "-printf", "-H", "-L"},
                  {"-name", "-iname", "-path", "-ipath",
                   "-wholename", "-iwholename", "-regex", "-iregex"}}},
        // grep: -e/--regexp pattern text, -f/--file pattern file list.
        // -R follows every symlink found while recursing (-r does not),
        // so only the follow-spellings gate; -L/-l (file lists) are
        // unrelated single letters and must NOT gate.
        {"grep", {false, {"-R", "--dereference"},
                  {"-e", "--regexp", "-f", "--file"}, true}},
        {"egrep", {false, {"-R", "--dereference"},
                   {"-e", "--regexp", "-f", "--file"}, true}},
        {"fgrep", {false, {"-R", "--dereference"},
                   {"-e", "--regexp", "-f", "--file"}, true}},
        // rg: -e pattern, -r/--replace replacement text, -f pattern file
        // list. --follow traverses symlinked directories; --pre pipes every
        // searched file through an arbitrary command (process spawn, not a
        // read).
        {"rg", {false, {"--follow", "--pre"},
                {"-e", "--regexp", "-r", "--replace", "-f", "--file"},
                true}},
        // sort: -k/--key and -t/--field-separator are sort-spec data;
        // -S/--buffer-size is a memory size. -o/--output is the file write
        // the redirect scanner cannot see, and --compress-program executes
        // an arbitrary program on temp files (process spawn).
        {"sort", {false, {"-o", "--output", "--compress-program"},
                  {"-k", "--key", "-t", "--field-separator",
                   "-S", "--buffer-size"}, true}},
        // fd/fdfind: -x/-X execute a command on every match (process
        // spawn, not a read; the same family as rg --pre).
        {"fd", {false, {"-x", "-X", "--exec", "--exec-batch"}}},
        {"fdfind", {false, {"-x", "-X", "--exec", "--exec-batch"}}},
        // man: -P/--pager runs the pager command through a shell; -H/--html
        // runs a browser command. Everything else (sections, -a, -l, -w)
        // is a read.
        {"man", {false, {"-P", "-H", "--pager", "--html"}}},
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
        // git grep -O[<pager>] opens the matched files in a pager command
        // executed through a shell - command data that smuggles execution
        // regardless of where stdout lands. The lowercase -o (--only-
        // matching) is unrelated and stays allowed.
        if (sub == "grep") {
            for (const std::string& word : rest) {
                if (starts_with(word, "-O") ||
                    starts_with(word, "--open-files-in-pager")) {
                    return {false, "git grep -O runs a pager command"};
                }
            }
        }
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

// Deny scan for a flag-policy command's argument vector. Data options
// (the per-command data_options allowlist) are exempt together with the
// argument they consume, so command data is never classified by the
// write-flag tables; short bundles are walked letters-wise per getopt
// semantics (an argument-taking letter consumes the rest of the bundle, or
// the next word when it is last in the bundle). Everything the walk cannot
// attribute still goes through the deny tables - fail-closed: unknown
// shapes keep gating, including bare words (icacls slash switches).
Verdict scan_arguments(const std::string& name, const FlagPolicy& policy,
                       const std::vector<std::string>& rest, Shell shell) {
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string word = normalize_name(rest[i], shell);
        if (policy.allow_end_of_options && word == "--") {
            break;  // everything after is an operand or data
        }
        bool data = false;
        bool attached = false;
        for (const std::string& flag : policy.data_flags) {
            if (word == flag || starts_with(word, flag + "=")) {
                data = true;
                attached = starts_with(word, flag + "=");
                break;
            }
        }
        if (data) {
            // An option without an attached value consumes the next word
            // as its value; the value is command data too, not an option
            // to classify (`grep --regexp -R file`).
            if (!attached && i + 1 < rest.size()) {
                ++i;
            }
            continue;
        }
        // Short-option bundle.
        if (word.size() >= 2 && word[0] == '-' && word[1] != '-') {
            std::size_t j = 1;
            bool data_letter = false;
            for (; j < word.size(); ++j) {
                const std::string letter = std::string("-") + word[j];
                if (std::find(policy.data_flags.begin(),
                              policy.data_flags.end(), letter) !=
                    policy.data_flags.end()) {
                    data_letter = true;
                    break;
                }
                if (std::find(policy.deny_flags.begin(),
                              policy.deny_flags.end(), letter) !=
                    policy.deny_flags.end()) {
                    return {false, name + " write flag: " + word};
                }
            }
            if (data_letter) {
                if (j + 1 >= word.size() && i + 1 < rest.size()) {
                    ++i;  // the value comes as the next word (-ne <pattern>)
                }
            } else {
                // The whole bundle walked without hitting a data letter:
                // it may still carry a long write flag with a bundled
                // suffix (`-delete` on its own, `-fprintX`), so the deny
                // tables apply by prefix.
                for (const std::string& deny : policy.deny_flags) {
                    if (flag_hit(word, deny)) {
                        return {false, name + " write flag: " + word};
                    }
                }
            }
            continue;
        }
        // Long options and bare words keep the plain deny-table prefix
        // match (bundled suffixes: `-i.bak` matches `-i`).
        for (const std::string& deny : policy.deny_flags) {
            if (flag_hit(word, deny)) {
                return {false, name + " write flag: " + word};
            }
        }
    }
    return {true, ""};
}

// ── sed script classification ───────────────────────────────────────────
// sed's inline script is command data, but parts of it escape stream
// editing: the s-command w flag and the w/W commands write files, r/R
// read arbitrary files into the stream (R asynchronously), and e (bare
// or as an s flag) runs a shell command. A read-only sed classification
// therefore requires the script itself to be provably pure stream
// editing: every command must be on the safe list, and any parse
// ambiguity (unterminated regex, unknown letter, exotic address shape)
// gates the whole call - fail-closed beats precise.
//
// Verified against GNU sed: a bare a/i/c followed by a newline lets the
// NEXT line execute as its own command, so a text command must never
// swallow the following line without a backslash; a `-` at command
// position makes GNU sed fail the whole compile ("unknown command: `-`"),
// which is inert, so leading dashes on a script word stay classifiable.
class SedScriptScanner {
public:
    explicit SedScriptScanner(const std::string& script)
        : s_(script), n_(script.size()) {}

    bool safe() {
        while (ok_) {
            skip_separators();
            if (i_ >= n_) break;
            parse_command();
        }
        if (ok_ && depth_ != 0) {
            fail("unterminated sed block");
        }
        return ok_;
    }

    const std::string& reason() const { return reason_; }

private:
    void fail(std::string why) {
        ok_ = false;
        reason_ = std::move(why);
    }

    static bool is_hspace(char c) { return c == ' ' || c == '\t'; }

    static bool is_digit(char c) {
        return std::isdigit(static_cast<unsigned char>(c)) != 0;
    }

    void skip_hspace() {
        while (i_ < n_ && is_hspace(s_[i_])) ++i_;
    }

    // Between commands: blanks, newlines, and repeated `;` are inert.
    void skip_separators() {
        while (i_ < n_) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                c == ';') {
                ++i_;
            } else {
                break;
            }
        }
    }

    void skip_digits() {
        while (i_ < n_ && is_digit(s_[i_])) ++i_;
    }

    // One delimited body up to an unescaped `delim`, honoring backslash
    // escapes; with `brackets`, bracket expressions protect the
    // delimiter inside [...] (regexes only - y sets and s replacements
    // have no character classes). False on ambiguity.
    bool read_body(char delim, bool brackets) {
        bool in_bracket = false;
        bool bracket_first = true;
        while (i_ < n_) {
            const char c = s_[i_];
            if (c == '\\') {
                ++i_;
                if (i_ >= n_) return false;  // dangling escape
                ++i_;
                continue;
            }
            if (!in_bracket && c == delim) {
                ++i_;
                return true;
            }
            if (brackets) {
                if (in_bracket) {
                    if (c == ']' && !bracket_first) {
                        in_bracket = false;
                        bracket_first = true;
                        ++i_;
                        continue;
                    }
                    bracket_first = false;
                    ++i_;
                    continue;
                }
                if (c == '[') {
                    in_bracket = true;
                    bracket_first = true;
                    ++i_;
                    if (i_ < n_ && s_[i_] == '^') ++i_;
                    continue;
                }
            }
            ++i_;
        }
        return false;  // ran off the end: unterminated
    }

    // One line number / `$` / /re/ / \cregc address at the current
    // position. False on ambiguity.
    bool read_one_address(bool& was_regex) {
        was_regex = false;
        if (i_ >= n_) return false;
        const char c = s_[i_];
        if (c == '$') {
            ++i_;
            return true;
        }
        if (is_digit(c)) {
            skip_digits();
            return true;
        }
        if (c == '/') {
            ++i_;
            was_regex = read_body('/', true);
            return was_regex;
        }
        if (c == '\\') {
            ++i_;
            if (i_ >= n_) return false;
            const char d = s_[i_++];
            was_regex = read_body(d, true);
            return was_regex;
        }
        return false;
    }

    // Addresses before a command: `10`, `$`, /re/, \cregc, ranges via
    // `,` + second address or `,+N`, `first~step`, then an optional `!`
    // negation (GNU case modifiers I/M may interleave with it). An
    // absent address is fine - the command letter follows.
    bool read_addresses(bool& negated, bool& was_regex) {
        negated = false;
        was_regex = false;
        skip_hspace();
        if (i_ >= n_) {
            fail("sed script ends after a separator");
            return false;
        }
        if (!is_digit(s_[i_]) && s_[i_] != '$' && s_[i_] != '/' &&
            s_[i_] != '\\') {
            return true;
        }
        if (!read_one_address(was_regex)) {
            fail("unparseable sed address");
            return false;
        }
        // Optional second address / ~step / +offset.
        for (;;) {
            skip_hspace();
            if (i_ >= n_) break;
            const char c = s_[i_];
            if (c == '~') {
                ++i_;
                skip_hspace();
                if (i_ >= n_ || !is_digit(s_[i_])) {
                    fail("sed ~step without a line number");
                    return false;
                }
                skip_digits();
                break;
            }
            if (c == '+') {
                ++i_;
                if (!read_step_count()) return false;
                continue;
            }
            if (c != ',') break;
            ++i_;
            skip_hspace();
            if (i_ >= n_) {
                fail("sed range without a second address");
                return false;
            }
            if (s_[i_] == '+') {
                ++i_;
                if (!read_step_count()) return false;
                continue;
            }
            bool second_was_regex = false;
            if (!read_one_address(second_was_regex)) {
                fail("unparseable sed address");
                return false;
            }
            was_regex = second_was_regex;
        }
        // GNU case modifiers and `!` may interleave (e.g. /re/I!d).
        for (;;) {
            skip_hspace();
            if (i_ >= n_) break;
            const char c = s_[i_];
            if (was_regex && (c == 'I' || c == 'M')) {
                ++i_;
                continue;
            }
            if (c == '!') {
                negated = true;
                ++i_;
                continue;
            }
            break;
        }
        return true;
    }

    // The digit count after `~`/`+` in a step/offset address.
    bool read_step_count() {
        if (i_ >= n_ || !is_digit(s_[i_])) {
            fail("sed step/offset without a line count");
            return false;
        }
        skip_digits();
        return true;
    }

    // a/i/c text: GNU one-liner (`a text`), POSIX `a\text`, or `a\` with
    // the text starting on the next line (backslash-newline
    // continuation). Bare `a` with no text is inert too (GNU sed rejects
    // it at compile time). The text NEVER extends past an unescaped
    // newline: GNU sed executes the next line as its own command, so the
    // scan must resume there (without a backslash) rather than swallow it.
    bool read_text_command() {
        skip_hspace();
        bool saw_backslash = false;
        if (i_ < n_ && s_[i_] == '\\') {
            saw_backslash = true;
            ++i_;
            skip_hspace();
        }
        if (i_ < n_ && (s_[i_] == '\n' || s_[i_] == '\r')) {
            if (!saw_backslash) {
                return true;  // next line runs as its own command
            }
            ++i_;  // continuation: the text starts on the next line
        }
        return read_text_to_eol();
    }

    // a/i/c text runs to an unescaped end of line; a backslash before
    // the newline continues the text on the next line. End of script
    // also ends the text (a trailing backslash is inert: GNU sed
    // accepts it with empty text).
    bool read_text_to_eol() {
        while (i_ < n_) {
            const char c = s_[i_];
            if (c == '\\') {
                ++i_;
                if (i_ >= n_) return true;
                ++i_;  // escaped char, or escaped newline (continuation)
                continue;
            }
            if (c == '\n' || c == '\r') return true;
            ++i_;
        }
        return true;
    }

    // Label tails (:label, b [label], t [label]): the label runs to the
    // next `;`, `}`, or end of line. Label content is inert either way.
    bool read_label_tail() {
        skip_hspace();
        while (i_ < n_) {
            const char c = s_[i_];
            if (c == ';' || c == '}' || c == '\n' || c == '\r') break;
            ++i_;
        }
        return true;
    }

    // Optional digit tails: q/Q [exit-code], l [width]. GNU sed rejects
    // anything else there ("extra characters after command"), so
    // non-digits gate conservatively.
    bool read_count_tail() {
        skip_hspace();
        skip_digits();
        return expect_command_end();
    }

    bool expect_command_end() {
        skip_hspace();
        if (i_ >= n_) return true;
        const char c = s_[i_];
        if (c == ';' || c == '}' || c == '\n' || c == '\r') return true;
        fail("unexpected character after sed command: " +
             std::string(1, c));
        return false;
    }

    bool read_delim(char* delim, const char* cmd) {
        if (i_ >= n_) {
            fail(std::string("sed ") + cmd + " command without a delimiter");
            return false;
        }
        char d = s_[i_++];
        if (d == '\\') {
            if (i_ >= n_) {
                fail(std::string("sed ") + cmd +
                     " command without a delimiter");
                return false;
            }
            d = s_[i_++];
        }
        *delim = d;
        return true;
    }

    // s/regex/replacement/flags: any single-character delimiter (with
    // an optional leading backslash, GNU custom-delim form). Flags:
    // digits, g, p, i, I, m, M; ANY other alpha flag (w = file write,
    // e = shell execute, or unknown) gates.
    bool read_s_command() {
        char delim = 0;
        if (!read_delim(&delim, "s")) return false;
        if (!read_body(delim, true)) {
            fail("unterminated sed s regex");
            return false;
        }
        if (!read_body(delim, false)) {
            fail("unterminated sed s replacement");
            return false;
        }
        for (;;) {
            if (i_ >= n_) return true;
            const char c = s_[i_];
            if (c == ';' || c == '}' || c == '\n' || c == '\r') return true;
            if (is_hspace(c) || is_digit(c)) {
                ++i_;
                continue;
            }
            if (c == 'g' || c == 'p' || c == 'i' || c == 'I' || c == 'm' ||
                c == 'M') {
                ++i_;
                continue;
            }
            fail(std::string("unsafe s flag: ") + c);
            return false;
        }
    }

    // y/set1/set2/ - no flags; the next thing must be a separator.
    bool read_y_command() {
        char delim = 0;
        if (!read_delim(&delim, "y")) return false;
        if (!read_body(delim, false) || !read_body(delim, false)) {
            fail("unterminated sed y set");
            return false;
        }
        return expect_command_end();
    }

    void parse_command() {
        bool negated = false;
        bool was_regex = false;
        if (!read_addresses(negated, was_regex)) return;
        (void)negated;  // negation binds to the command below either way
        skip_hspace();
        if (i_ >= n_) return;  // trailing address: empty command, inert
        const char c = s_[i_];
        if (c == ';' || c == '\n' || c == '\r') {
            return;  // empty command; the separator is skipped next round
        }
        while (i_ < n_ && s_[i_] == '-') {
            // No sed dialect has `-` syntax at command position; a script
            // with one fails the compile (verified on GNU sed), so the
            // dashes are inert and the letter after them decides.
            ++i_;
        }
        if (i_ >= n_) return;
        const char cmd = s_[i_++];
        switch (cmd) {
        case '{':
            ++depth_;
            return;
        case '}':
            if (depth_ == 0) {
                fail("unbalanced sed braces");
                return;
            }
            --depth_;
            return;
        case ':':
        case 'b':
        case 't':
            read_label_tail();
            return;
        case 'a':
        case 'i':
        case 'c':
            read_text_command();
            return;
        case 's':
            read_s_command();
            return;
        case 'y':
            read_y_command();
            return;
        case 'q':
        case 'Q':
        case 'l':
            read_count_tail();
            return;
        case 'd':
        case 'n':
        case 'N':
        case 'p':
        case 'P':
        case '=':
        case 'F':
        case 'h':
        case 'H':
        case 'g':
        case 'G':
        case 'x':
        case 'z':
            return;
        case 'w':
        case 'W':
        case 'r':
        case 'R':
        case 'e':
            fail(std::string("sed command writes or executes: ") + cmd);
            return;
        default:
            fail(std::string("unrecognized sed command: ") + cmd);
            return;
        }
    }

    const std::string& s_;
    std::size_t n_;
    std::size_t i_ = 0;
    int depth_ = 0;
    bool ok_ = true;
    std::string reason_;
};

// The sed script: every -e/--expression value, plus the first bare
// positional argument when no -e and no -f was given (GNU sed's rule:
// "the first non-option argument is taken as the sed script"). Input
// file operands are inert for classification (sed only reads them).
// -f/--file takes the script from an arbitrary file - an operand we do
// not vouch for - so it gates outright. The -i/--in-place deny is
// handled by the flag tables before this runs.
Verdict evaluate_sed(const std::vector<std::string>& rest, Shell shell) {
    (void)shell;  // the script language is the same on every shell
    std::string script;
    bool saw_e = false;
    bool end_of_options = false;
    bool positional_taken = false;
    auto add_part = [&](const std::string& part) {
        // GNU sed concatenates the expressions with newlines between
        // them, so the scanner sees exactly what sed would.
        if (!script.empty()) script += '\n';
        script += part;
    };
    for (std::size_t k = 0; k < rest.size(); ++k) {
        const std::string& word = rest[k];
        if (!end_of_options && word == "--") {
            end_of_options = true;
            continue;
        }
        if (!end_of_options && word.size() >= 2 && word[0] == '-' &&
            word[1] == '-') {
            std::string body = word.substr(2);
            std::string value;
            bool attached = false;
            const std::size_t eq = body.find('=');
            if (eq != std::string::npos) {
                value = body.substr(eq + 1);
                body.resize(eq);
                attached = true;
            }
            if (body == "file") {
                return {false,
                        "sed -f/--file reads an unvouched script file"};
            }
            if (body == "expression") {
                saw_e = true;
                if (attached) {
                    add_part(value);
                } else if (k + 1 < rest.size()) {
                    add_part(rest[++k]);
                } else {
                    return {false, "sed --expression without a value"};
                }
                continue;
            }
            // Inert no-argument options; anything unrecognized gates.
            static const std::set<std::string> inert_long = {
                "quiet", "silent", "regexp-extended", "separate",
                "unbuffered", "null-data", "posix", "binary",
            };
            if (!inert_long.count(body)) {
                return {false, "unknown sed option: --" + body};
            }
            continue;
        }
        if (!end_of_options && word.size() >= 2 && word[0] == '-' &&
            word[1] != '-') {
            // Short bundle, walked letters-wise per getopt semantics: an
            // argument-taking letter (e) eats the rest of the bundle, or
            // the next word when it is last.
            std::size_t j = 1;
            for (; j < word.size(); ++j) {
                const char letter = word[j];
                if (letter == 'f') {
                    return {false,
                            "sed -f/--file reads an unvouched script file"};
                }
                if (letter == 'e') {
                    saw_e = true;
                    if (j + 1 < word.size()) {
                        add_part(word.substr(j + 1));
                    } else if (k + 1 < rest.size()) {
                        add_part(rest[++k]);
                    } else {
                        return {false, "sed -e without a value"};
                    }
                    break;
                }
                if (letter != 'n' && letter != 'E' && letter != 'r' &&
                    letter != 's' && letter != 'u' && letter != 'z' &&
                    letter != 'b') {
                    return {false,
                            std::string("unknown sed option: -") + letter};
                }
            }
            continue;
        }
        // A bare operand: the script when no -e was given, else an input
        // file (sed reads it; reading is what read-only means).
        if (!saw_e && !positional_taken) {
            add_part(word);
            positional_taken = true;
        }
    }
    if (script.empty()) {
        return {false, "sed without a script gates"};
    }
    SedScriptScanner scanner(script);
    if (!scanner.safe()) {
        return {false, "sed script gates: " + scanner.reason()};
    }
    return {true, ""};
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
        const Verdict args = scan_arguments(name, it->second, rest, shell);
        if (!args.read_only) {
            return args;
        }
        if (name == "sed") {
            // The flag tables alone are not enough for sed: the inline
            // script data can write files or run a shell.
            return evaluate_sed(rest, shell);
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
//
// Single quotes are fully literal in /bin/sh, so inside them the shell
// expansion characters ($ ` % ~ !) are inert and only gate for the
// Posix flavor; cmd.exe has no single-quote quoting (and %-expansion is
// parsed before quoting), so the Cmd flavor keeps the strict union.
std::size_t read_word(const std::string& s, std::size_t i,
                      std::string& word, bool& quoted, Shell shell) {
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
        if ((quote == '"' || shell == Shell::Cmd) &&
            (c == '$' || c == '`' || c == '%' || c == '~' || c == '!')) {
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
                const std::size_t end = read_word(s, j, target, target_quoted,
                                                  shell);
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
        const std::size_t end = read_word(s, i, word, word_q, shell);
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
