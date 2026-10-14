#include "nyaxus/denyguard.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "nyaxus/json.hpp"

namespace nyaxus {
namespace denyguard {
namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// The executable name without any path prefix (`/bin/rm` -> `rm`).
std::string base_name(const std::string& word) {
    const std::size_t slash = word.find_last_of("/\\");
    return slash == std::string::npos ? word : word.substr(slash + 1);
}

// ── Secret-argument scan ──────────────────────────────────────────────
// Runs over the raw argument JSON as text: a credential-shaped value
// anywhere in the call is a denial, whatever the tool.

bool token_charset(const std::string& s, std::size_t start, std::size_t end) {
    for (std::size_t i = start; i < end; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c) && c != '.' && c != '_' && c != '-' &&
            c != '+' && c != '/' && c != '=') {
            return false;
        }
    }
    return true;
}

// `"key": "value"` with a string value of 8+ token-charset characters.
// Numeric values (token_count: 5) and short values do not match.
bool secret_key_value(const std::string& raw, const std::string& key) {
    const std::string lower = to_lower(raw);
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = 0;
    while ((pos = lower.find(needle, pos)) != std::string::npos) {
        pos += needle.size();
        while (pos < raw.size() &&
               std::isspace(static_cast<unsigned char>(raw[pos]))) pos++;
        if (pos >= raw.size() || raw[pos] != ':') continue;
        pos++;
        while (pos < raw.size() &&
               std::isspace(static_cast<unsigned char>(raw[pos]))) pos++;
        if (pos >= raw.size() || raw[pos] != '"') continue;
        pos++;
        const std::size_t start = pos;
        while (pos < raw.size() && raw[pos] != '"') pos++;
        if (pos < raw.size() && pos - start >= 8 &&
            token_charset(raw, start, pos)) {
            return true;
        }
        pos++;
    }
    return false;
}

bool followed_by(const std::string& s, std::size_t prefix_end,
                 std::size_t min_len, bool upper_alnum_only) {
    if (prefix_end + min_len > s.size()) return false;
    for (std::size_t i = prefix_end; i < prefix_end + min_len; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const bool ok = upper_alnum_only
                            ? (std::isalnum(c) || c == '_' || c == '-')
                            : (std::isalnum(c) || c == '_' || c == '-' ||
                               c == '.' || c == '+' || c == '/' || c == '=');
        if (!ok) return false;
    }
    return true;
}

std::string value_pattern_reason(const std::string& s) {
    std::size_t pos;
    if ((pos = s.find("-----BEGIN")) != std::string::npos) {
        return "PEM block (private key material) in the arguments";
    }
    if ((pos = s.find("sk-")) != std::string::npos &&
        followed_by(s, pos + 3, 16, false)) {
        return "OpenAI-style API key (sk-…) in the arguments";
    }
    for (const char* prefix : {"ghp_", "gho_", "ghu_", "ghs_", "ghr_",
                               "github_pat_"}) {
        if ((pos = s.find(prefix)) != std::string::npos &&
            followed_by(s, pos + std::strlen(prefix), 30, true)) {
            return "GitHub token in the arguments";
        }
    }
    for (const char* prefix : {"xoxb-", "xoxa-", "xoxp-", "xoxr-", "xoxs-"}) {
        if (s.find(prefix) != std::string::npos) {
            return "Slack token in the arguments";
        }
    }
    if ((pos = s.find("AKIA")) != std::string::npos &&
        followed_by(s, pos + 4, 16, true)) {
        return "AWS access key id in the arguments";
    }
    if ((pos = s.find("AIza")) != std::string::npos &&
        followed_by(s, pos + 4, 30, false)) {
        return "Google API key in the arguments";
    }
    if ((pos = s.find("glpat-")) != std::string::npos &&
        followed_by(s, pos + 6, 10, false)) {
        return "GitLab personal access token in the arguments";
    }
    if ((pos = s.find("npm_")) != std::string::npos &&
        followed_by(s, pos + 4, 20, true)) {
        return "npm token in the arguments";
    }
    // Bearer <token>: at least 20 token-charset characters.
    for (const char* word : {"Bearer ", "bearer "}) {
        if ((pos = s.find(word)) != std::string::npos &&
            followed_by(s, pos + 7, 20, false)) {
            return "bearer token in the arguments";
        }
    }
    // JWT: three base64url segments, eyJ start.
    if ((pos = s.find("eyJ")) != std::string::npos &&
        followed_by(s, pos + 3, 10, true)) {
        const std::size_t dot1 = s.find('.', pos + 13);
        if (dot1 != std::string::npos && dot1 - (pos + 3) < 64 &&
            followed_by(s, dot1 + 1, 10, true)) {
            const std::size_t dot2 = s.find('.', dot1 + 11);
            if (dot2 != std::string::npos && dot2 - (dot1 + 1) < 64) {
                return "JWT in the arguments";
            }
        }
    }
    return "";
}

std::string secret_reason(const std::string& raw) {
    for (const char* key : {"api_key", "apikey", "api-key", "access_token",
                            "auth_token", "password", "passwd", "secret",
                            "authorization", "bearer", "token",
                            "client_secret", "private_key"}) {
        if (secret_key_value(raw, key)) {
            return std::string("credential-shaped value for '") + key +
                   "' in the arguments";
        }
    }
    return value_pattern_reason(raw);
}

// ── Obviously-destructive argv ────────────────────────────────────────
// Only the terminal tool. Dynamic syntax (unparseable) is a gate, not a
// guard: the guard denies what it can name.

const std::set<std::string>& root_targets() {
    static const std::set<std::string> targets = {
        "/",  "HOMEDIR",   "/*",        "HOMEDIR/*", "/root",
        "/home", "/etc", "/usr",  "/var",  "/bin",  "/sbin",
        "/lib",  "/lib64", "/boot", "/opt",  "/srv",  "/sys",
        "/dev",  "/proc",  "/tmp",
    };
    return targets;
}

std::string posix_reason(const std::vector<std::string>& words) {
    if (words.empty()) return "";
    const std::string name = base_name(words.front());

    if (name == "rm") {
        bool recursive = false;
        bool only_flags = true;
        for (std::size_t i = 1; i < words.size(); ++i) {
            const std::string& w = words[i];
            if (w == "--") {
                only_flags = false;
                continue;
            }
            if (!only_flags) continue;  // target
            if (w.size() > 1 && w.front() == '-') {
                if (w == "--recursive") {
                    recursive = true;
                } else {
                    for (const char c : w) {
                        if (c == 'r' || c == 'R') {
                            recursive = true;
                            break;
                        }
                    }
                }
            } else {
                only_flags = false;
            }
        }
        if (recursive) {
            for (std::size_t i = 1; i < words.size(); ++i) {
                const std::string& t = words[i];
                if (t.size() > 1 && t.front() == '-') continue;
                if (root_targets().count(t) > 0) {
                    const std::string shown =
                        t == "HOMEDIR" ? "~" : t == "HOMEDIR/*" ? "~/*" : t;
                    return "recursive rm of '" + shown +
                           "' (root/home-level target)";
                }
                // /home, /usr, ... as the prefix of a longer path are fine;
                // only the bare targets above (plus trailing /*) deny.
            }
        }
        return "";
    }

    if (starts_with(name, "mkfs")) {
        return name + " formats a block device";
    }
    if (name == "dd") {
        for (std::size_t i = 1; i < words.size(); ++i) {
            if (starts_with(words[i], "of=/dev/")) {
                const std::string dev = words[i].substr(8);
                if (dev != "null" && dev != "stdout" && dev != "stderr" &&
                    dev != "tty" && dev != "fd") {
                    return "dd writing to /dev/" + dev + " (block device)";
                }
            }
        }
        return "";
    }
    if (name == "shutdown" || name == "reboot" || name == "halt" ||
        name == "poweroff") {
        return name + " cycles the machine";
    }
    if (name == "init") {
        for (std::size_t i = 1; i < words.size(); ++i) {
            if (words[i] == "0" || words[i] == "6") {
                return "init " + words[i] + " changes the run level";
            }
        }
    }
    if (name == "wipefs" || name == "fdisk" || name == "parted" ||
        name == "sfdisk") {
        return name + " rewrites disk metadata";
    }
    if (name == "git" && words.size() > 1 && words[1] == "push") {
        for (std::size_t i = 2; i < words.size(); ++i) {
            if (words[i] == "--force" || words[i] == "-f") {
                return "git push --force overwrites remote history";
            }
        }
    }
    return "";
}

std::string cmd_reason(const std::vector<std::string>& words) {
    if (words.empty()) return "";
    const std::string name = to_lower(base_name(words.front()));

    if (name == "format") {
        for (std::size_t i = 1; i < words.size(); ++i) {
            const std::string& w = to_lower(words[i]);
            if (w.size() == 2 && std::isalpha(static_cast<unsigned char>(w[0])) &&
                w[1] == ':') {
                return "format " + words[i] + " formats a volume";
            }
        }
    }
    if (name == "diskpart") {
        return "diskpart rewrites disk layout";
    }
    if (name == "shutdown") {
        return "shutdown cycles the machine";
    }
    if (name == "del" || name == "erase" || name == "rd" || name == "rmdir") {
        bool has_s = false, has_q = false;
        for (std::size_t i = 1; i < words.size(); ++i) {
            const std::string& w = to_lower(words[i]);
            if (w == "/s" || w == "/s ") has_s = true;
            if (w == "/q" || w == "/q ") has_q = true;
        }
        if (has_s && has_q) {
            return name + " /s /q deletes recursively without prompting";
        }
    }
    return "";
}

std::string destructive_reason(const std::string& command,
                               argvpolicy::Shell shell) {
    // Tilde is home-dir expansion, not dynamic syntax: normalize it to a
    // neutral token so the parser can see through it (the guard is
    // conservative - a miss is a gate, never a false pass).
    std::string normalized;
    normalized.reserve(command.size() + 16);
    for (char c : command) {
        if (c == '~') normalized += "HOMEDIR";
        else normalized += c;
    }
    const auto parsed = argvpolicy::parse(normalized);
    if (!parsed.ok) return "";
    for (const auto& sc : parsed.commands) {
        const std::string reason =
            shell == argvpolicy::Shell::Cmd ? cmd_reason(sc.words)
                                            : posix_reason(sc.words);
        if (!reason.empty()) return reason;
    }
    return "";
}

}  // namespace

Verdict evaluate(const std::string& tool, const std::string& arguments,
                 argvpolicy::Shell shell) {
    if (tool == "terminal") {
        std::string command;
        try {
            const json::Value args = json::parse(arguments);
            command = args["command"].as_string();
        } catch (...) {
            command.clear();
        }
        if (!command.empty()) {
            const std::string reason = destructive_reason(command, shell);
            if (!reason.empty()) {
                return {true, reason};
            }
        }
    }
    const std::string reason = secret_reason(arguments);
    if (!reason.empty()) {
        return {true, reason};
    }
    return {false, ""};
}

}  // namespace denyguard
}  // namespace nyaxus
