#include "nyaxus/fingerprint.hpp"

#include <algorithm>
#include <cctype>
#include <set>

#include "nyaxus/argvpolicy.hpp"
#include "nyaxus/json.hpp"

namespace nyaxus {
namespace fingerprint {
namespace {

std::string to_lower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool equal_fold(const std::string& a, const std::string& b) {
    return to_lower(a) == to_lower(b);
}

// A bare command name: no path separators, no drive letter, no
// relative/home prefix. Path-like names (./x, /x, C:\x) are never
// pattern-allowable - they name a specific file, not a stable tool.
bool is_bare_name(const std::string& word) {
    if (word.empty()) return false;
    if (word.find('/') != std::string::npos) return false;
    if (word.find('\\') != std::string::npos) return false;
    if (word.find(':') != std::string::npos) return false;
    if (word.front() == '.' || word.front() == '~') return false;
    if (std::isdigit(static_cast<unsigned char>(word.front()))) return false;
    return true;
}

bool is_flag(const std::string& word) {
    return !word.empty() && word.front() == '-';
}

bool is_path_like(const std::string& word) {
    return word.find('/') != std::string::npos;
}

// The parent directory prefix (ending in '/') of a path-like word.
std::string parent_prefix(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == 0) return "/";
    if (slash == std::string::npos) return "";
    return path.substr(0, slash + 1);
}

// Longest common directory prefix of two path-like words, ending in '/'.
// "" when there is no shared directory.
std::string common_parent(const std::string& a, const std::string& b) {
    std::size_t len = 0;
    const std::size_t max_len = std::min(a.size(), b.size());
    while (len < max_len && a[len] == b[len]) ++len;
    while (len > 0 && a[len - 1] != '/') --len;
    if (len == 0) return a.front() == '/' ? "/" : "";
    return a.substr(0, len);
}

// Tools whose first positional is a subcommand: the pattern is
// `cmd sub` and later calls must name the same subcommand.
const std::set<std::string>& subcommand_tools() {
    static const std::set<std::string> tools = {
        "npm", "npx", "yarn", "pnpm", "bun", "cargo", "make", "pip", "pip3",
        "poetry", "uv", "go", "gradle", "mvn", "bundle", "rake", "composer",
        "dotnet",
    };
    return tools;
}

// Copy/move tools: the pattern is the common parent of src and dest, so an
// "always" approved for one tree does not cover the rest of the disk.
const std::set<std::string>& copy_tools() {
    static const std::set<std::string> tools = {"cp", "mv", "rsync"};
    return tools;
}

// git global flags that take a value: the flag and its value are both
// skipped when hunting for the subcommand.
bool git_flag_with_value(const std::string& word) {
    static const std::set<std::string> flags = {
        "-C",     "--git-dir",   "--work-tree",   "-c",
        "--exec-path", "--namespace", "--config-env", "--shallow-file",
        "--attr-source", "--super-prefix"};
    return flags.count(word) > 0;
}

// First non-flag word of a simple command (git-aware: value-taking global
// flags skip their value too). Empty when there is none.
std::string first_positional(const std::vector<std::string>& words, bool git) {
    for (std::size_t i = 1; i < words.size(); ++i) {
        const std::string& w = words[i];
        if (is_flag(w)) {
            if (git && git_flag_with_value(w) && i + 1 < words.size()) ++i;
            continue;
        }
        return w;
    }
    return "";
}

std::string extract_arg(const std::string& args_json, const std::string& key) {
    if (args_json.empty()) return "";
    try {
        const json::Value args = json::parse(args_json);
        if (args.type() == json::Value::Type::Object && args.has(key)) {
            return args.at(key).as_string("");
        }
    } catch (...) {
    }
    return "";
}

// Lowercased host of an http(s) URL ("" when not extractable).
std::string url_host(const std::string& url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) return "";
    const std::size_t start = scheme + 3;
    std::size_t end = url.size();
    for (const char stop : {'/', '?', '#'}) {
        const std::size_t found = url.find(stop, start);
        if (found != std::string::npos && found < end) end = found;
    }
    // Strip a trailing port: host:port -> host.
    const std::size_t colon = url.rfind(':', end);
    if (colon != std::string::npos && colon > start) end = colon;
    if (end <= start) return "";
    return to_lower(url.substr(start, end - start));
}

std::string join_patterns(const std::vector<std::string>& patterns) {
    std::string label;
    for (const std::string& p : patterns) {
        if (!label.empty()) label += ", ";
        label += p;
    }
    return label;
}

}  // namespace

Rule derive(const std::string& tool, const std::string& args_json,
            bool posix_shell) {
    Rule rule;
    rule.tool = tool;
    if (tool == "terminal") {
        if (!posix_shell) {
            // cmd.exe has no POSIX chaining: a pipeline pattern cannot be
            // expressed, so the option degrades to whole-tool.
            rule.label = "whole tool (cmd shell)";
            return rule;
        }
        const std::string command = extract_arg(args_json, "command");
        const argvpolicy::ParsedCommand parsed = argvpolicy::parse(command);
        if (!parsed.ok) {
            rule.label = "whole tool (" + parsed.reason + ")";
            return rule;
        }
        rule.patterns.reserve(parsed.commands.size());
        for (const auto& cmd : parsed.commands) {
            const std::vector<std::string>& words = cmd.words;
            if (words.empty() || !is_bare_name(words.front())) {
                rule = Rule{tool, {}, "whole tool (path-like command)"};
                return rule;
            }
            const std::string name = words.front();
            if (equal_fold(name, "git")) {
                const std::string sub = first_positional(words, /*git=*/true);
                if (sub.empty()) {
                    rule = Rule{tool, {}, "whole tool (no git subcommand)"};
                    return rule;
                }
                rule.patterns.push_back("git " + sub);
            } else if (subcommand_tools().count(name) > 0) {
                const std::string sub = first_positional(words, false);
                rule.patterns.push_back(sub.empty() ? name : name + " " + sub);
            } else {
                const std::string first = first_positional(words, false);
                if (first.empty()) {
                    rule.patterns.push_back(name);
                } else if (is_path_like(first)) {
                    std::string parent;
                    if (copy_tools().count(name) > 0) {
                        for (std::size_t i = 2; i < words.size(); ++i) {
                            if (is_path_like(words[i])) {
                                parent = common_parent(first, words[i]);
                                break;
                            }
                        }
                    }
                    if (parent.empty()) parent = parent_prefix(first);
                    if (parent.empty()) {
                        rule = Rule{tool, {}, "whole tool (no path parent)"};
                        return rule;
                    }
                    rule.patterns.push_back("^" + parent);
                } else {
                    rule.patterns.push_back(name);
                }
            }
        }
        rule.label = join_patterns(rule.patterns);
        return rule;
    }
    if (tool == "web.fetch") {
        const std::string host = url_host(extract_arg(args_json, "url"));
        if (host.empty()) {
            rule.label = "whole tool (no url host)";
            return rule;
        }
        rule.patterns.push_back(host);
        rule.label = host;
        return rule;
    }
    rule.label = "whole tool";
    return rule;
}

bool matches(const std::string& tool, const std::string& patterns,
             const std::string& args_json) {
    if (patterns.empty()) return true;  // whole tool
    const std::vector<std::string> stored = parse_patterns(patterns);
    if (stored.empty()) return true;
    if (tool == "web.fetch") {
        if (stored.size() != 1) return false;
        return url_host(extract_arg(args_json, "url")) == stored[0];
    }
    if (tool != "terminal") return false;
    const std::string command = extract_arg(args_json, "command");
    const argvpolicy::ParsedCommand parsed = argvpolicy::parse(command);
    if (!parsed.ok) return false;
    if (parsed.commands.size() != stored.size()) return false;
    for (std::size_t i = 0; i < parsed.commands.size(); ++i) {
        const std::vector<std::string>& words = parsed.commands[i].words;
        if (words.empty()) return false;
        const std::string& pattern = stored[i];
        if (pattern.size() > 1 && pattern.front() == '^' &&
            pattern.back() == '/') {
            // Path-scoped: every non-flag argument must stay under the
            // parent directory.
            bool any = false;
            for (std::size_t j = 1; j < words.size(); ++j) {
                if (is_flag(words[j])) continue;
                any = true;
                if (words[j].rfind(pattern.substr(1), 0) != 0) return false;
            }
            if (!any) return false;
        } else if (pattern.find(' ') != std::string::npos) {
            // cmd sub: the name matches (case-insensitive) and the first
            // positional (git-aware) equals the sub (case-sensitive).
            const std::size_t space = pattern.find(' ');
            const std::string cmd = pattern.substr(0, space);
            const std::string sub = pattern.substr(space + 1);
            if (!equal_fold(words.front(), cmd)) return false;
            if (first_positional(words, equal_fold(cmd, "git")) != sub) {
                return false;
            }
        } else {
            // Whole command: the name matches (case-insensitive).
            if (!equal_fold(words.front(), pattern)) return false;
        }
    }
    return true;
}

std::string serialize_patterns(const std::vector<std::string>& patterns) {
    if (patterns.empty()) return "";
    json::Array array;
    array.reserve(patterns.size());
    for (const std::string& p : patterns) array.push_back(json::Value(p));
    return json::stringify(json::Value(std::move(array)));
}

std::vector<std::string> parse_patterns(const std::string& serialized) {
    std::vector<std::string> patterns;
    if (serialized.empty()) return patterns;
    try {
        const json::Value value = json::parse(serialized);
        if (value.type() == json::Value::Type::Array) {
            for (const json::Value& entry : value.as_array()) {
                patterns.push_back(entry.as_string(""));
            }
        }
    } catch (...) {
    }
    return patterns;
}

std::string label_for(const std::string& serialized_patterns) {
    if (serialized_patterns.empty()) return "whole tool";
    const std::vector<std::string> patterns =
        parse_patterns(serialized_patterns);
    if (patterns.empty()) return "whole tool";
    return join_patterns(patterns);
}

}  // namespace fingerprint
}  // namespace nyaxus
