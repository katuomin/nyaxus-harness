// Fingerprint rulebook: argument-fingerprint derivation and matching for
// the tiered always-allow (design: docs/fingerprint-rulebook.md).
//
// Pure logic - no model artifacts, no daemon. Terminal patterns come from
// the shared shell parse (every pipeline subcommand must match), path
// patterns scope to a directory (copy/move use the common parent), and
// web.fetch patterns are URL hosts. Not-pattern-able calls degrade to
// whole-tool: dynamic expansion, parse failure, path-like command names,
// and the cmd.exe shell (no POSIX chaining).
#include <iostream>
#include <string>
#include <vector>

#include "nyaxus/fingerprint.hpp"

using namespace nyaxus;
namespace fp = nyaxus::fingerprint;

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
    std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
    std::cout.flush();
    if (!cond) g_fail++;
}

static void section(const std::string& name) {
    std::cout << "\n== " << name << " ==\n";
    std::cout.flush();
}

static fp::Rule derive_terminal(const std::string& command,
                                bool posix_shell = true) {
    return fp::derive("terminal", "{\"command\":" +
                                     std::string("\"") + command + "\"}",
                      posix_shell);
}

static std::string patterns_of(const fp::Rule& rule) {
    return fp::serialize_patterns(rule.patterns);
}

int main() {
    // ── derive: terminal subcommand patterns ─────────────────────────────
    section("derive: terminal subcommand patterns");
    {
        const fp::Rule r = derive_terminal("git status");
        check(r.patterns.size() == 1 && r.patterns[0] == "git status",
              "git status -> 'git status'");
        check(r.label == "git status", "label is the joined patterns");
    }
    {
        const fp::Rule r = derive_terminal("git --no-pager log --oneline");
        check(r.patterns.size() == 1 && r.patterns[0] == "git log",
              "git global flags skipped -> 'git log'");
    }
    {
        const fp::Rule r = derive_terminal("git -C /tmp/repo status");
        check(r.patterns.size() == 1 && r.patterns[0] == "git status",
              "value-taking git flag skips its value -> 'git status'");
    }
    {
        const fp::Rule r = derive_terminal("git");
        check(r.patterns.empty(), "bare git degrades to whole tool");
    }
    {
        const fp::Rule r = derive_terminal("npm test -- --coverage");
        check(r.patterns.size() == 1 && r.patterns[0] == "npm test",
              "npm test -> 'npm test'");
    }
    {
        const fp::Rule r = derive_terminal("npm");
        check(r.patterns.size() == 1 && r.patterns[0] == "npm",
              "bare npm -> whole command 'npm'");
    }
    {
        const fp::Rule r = derive_terminal("ls -la");
        check(r.patterns.size() == 1 && r.patterns[0] == "ls",
              "ls -la (no positional) -> whole command 'ls'");
    }
    {
        const fp::Rule r = derive_terminal("grep -r needle src");
        check(r.patterns.size() == 1 && r.patterns[0] == "grep",
              "non-path first positional -> whole command 'grep'");
    }

    // ── derive: terminal path patterns ───────────────────────────────────
    section("derive: terminal path patterns");
    {
        const fp::Rule r = derive_terminal("cat /a/b/c.txt");
        check(r.patterns.size() == 1 && r.patterns[0] == "^/a/b/",
              "path argument -> parent prefix '^/a/b/'");
    }
    {
        const fp::Rule r = derive_terminal("cp /a/b/x.txt /a/c/y.txt");
        check(r.patterns.size() == 1 && r.patterns[0] == "^/a/",
              "cp common parent of src+dest -> '^/a/'");
    }
    {
        const fp::Rule r = derive_terminal("cp /a/b/x.txt /tmp/y.txt");
        check(r.patterns.size() == 1 && r.patterns[0] == "^/",
              "cp across trees -> root '^/'");
    }
    {
        const fp::Rule r = derive_terminal("mv notes/draft.md notes/final.md");
        check(r.patterns.size() == 1 && r.patterns[0] == "^notes/",
              "relative path parent -> '^notes/'");
    }

    // ── derive: pipelines (every subcommand gets a pattern) ──────────────
    section("derive: pipelines");
    {
        const fp::Rule r = derive_terminal("git status && git log -n 3");
        check(r.patterns.size() == 2 && r.patterns[0] == "git status" &&
                  r.patterns[1] == "git log",
              "sequence -> one pattern per simple command");
    }
    {
        const fp::Rule r = derive_terminal("ls -la | grep daemon");
        check(r.patterns.size() == 2 && r.patterns[0] == "ls" &&
                  r.patterns[1] == "grep",
              "pipe -> one pattern per simple command");
    }

    // ── derive: not pattern-able -> whole tool ───────────────────────────
    section("derive: not pattern-able");
    {
        const fp::Rule r = derive_terminal("./script.sh");
        check(r.patterns.empty(), "relative path-like name -> whole tool");
    }
    {
        const fp::Rule r = derive_terminal("/bin/ls -la");
        check(r.patterns.empty(), "absolute path-like name -> whole tool");
    }
    {
        const fp::Rule r = derive_terminal("echo $HOME");
        check(r.patterns.empty(), "dynamic expansion -> whole tool");
    }
    {
        const fp::Rule r = derive_terminal("make -j4");
        check(r.patterns.size() == 1 && r.patterns[0] == "make",
              "make (no target) -> whole command");
    }
    {
        const fp::Rule r = derive_terminal("git status", /*posix_shell=*/false);
        check(r.patterns.empty(), "cmd shell -> whole tool (no POSIX chaining)");
    }
    {
        const fp::Rule r = fp::derive("memory.save", "{\"text\":\"x\"}", true);
        check(r.patterns.empty(), "other tools -> whole tool");
        check(r.label == "whole tool", "whole-tool label");
    }

    // ── derive: web.fetch host patterns ──────────────────────────────────
    section("derive: web.fetch host patterns");
    {
        const fp::Rule r =
            fp::derive("web.fetch", "{\"url\":\"https://example.com/a?b=1\"}",
                       true);
        check(r.patterns.size() == 1 && r.patterns[0] == "example.com",
              "https URL -> host");
    }
    {
        const fp::Rule r =
            fp::derive("web.fetch", "{\"url\":\"http://Example.COM:8080/x\"}",
                       true);
        check(r.patterns.size() == 1 && r.patterns[0] == "example.com",
              "host lowercased, port stripped");
    }
    {
        const fp::Rule r =
            fp::derive("web.fetch", "{\"url\":\"no-scheme-here\"}", true);
        check(r.patterns.empty(), "no scheme -> whole tool");
    }

    // ── matches: subcommand patterns ─────────────────────────────────────
    section("matches: subcommand patterns");
    {
        const std::string p = patterns_of(derive_terminal("git status"));
        check(fp::matches("terminal", p, "{\"command\":\"git status --short\"}"),
              "same sub, extra flags -> match");
        check(fp::matches("terminal", p, "{\"command\":\"git --no-pager status\"}"),
              "global flag before sub -> match");
        check(!fp::matches("terminal", p, "{\"command\":\"git log\"}"),
              "different sub -> no match");
        check(!fp::matches("terminal", p, "{\"command\":\"git STATUS\"}"),
              "sub is case-sensitive");
        check(fp::matches("terminal", p, "{\"command\":\"GIT status\"}"),
              "command name is case-insensitive");
        check(!fp::matches("terminal", p, "{\"command\":\"git status && git log\"}"),
              "more simple commands than stored -> no match");
        check(!fp::matches("terminal", p, "{\"command\":\"ls\"}"),
              "different tool command -> no match");
    }

    // ── matches: whole-tool and whole-command patterns ───────────────────
    section("matches: whole-tool and whole-command");
    {
        check(fp::matches("terminal", "", "{\"command\":\"anything at all\"}"),
              "empty pattern = whole tool");
        const std::string p = patterns_of(derive_terminal("ls -la"));
        check(fp::matches("terminal", p, "{\"command\":\"ls /x\"}"),
              "whole command: same name, any args -> match");
        check(!fp::matches("terminal", p, "{\"command\":\"cat x\"}"),
              "whole command: different name -> no match");
    }

    // ── matches: path patterns ───────────────────────────────────────────
    section("matches: path patterns");
    {
        const std::string p = patterns_of(derive_terminal("cat /a/b/c.txt"));
        check(fp::matches("terminal", p, "{\"command\":\"cat /a/b/other\"}"),
              "same parent -> match");
        check(fp::matches("terminal", p, "{\"command\":\"ls /a/b/x /a/b/y\"}"),
              "all args under parent -> match");
        check(!fp::matches("terminal", p, "{\"command\":\"cat /a/bc\"}"),
              "sibling prefix is not under parent");
        check(!fp::matches("terminal", p, "{\"command\":\"cat /a/b/c /tmp/x\"}"),
              "one arg escapes parent -> no match");
        check(!fp::matches("terminal", p, "{\"command\":\"ls\"}"),
              "no path args -> no match");
    }
    {
        const std::string p = patterns_of(derive_terminal("cp /a/b/x /a/c/y"));
        check(fp::matches("terminal", p, "{\"command\":\"cp /a/one /a/two\"}"),
              "cp within common parent -> match");
        check(!fp::matches("terminal", p, "{\"command\":\"cp /a/one /tmp/two\"}"),
              "cp escaping common parent -> no match");
    }

    // ── matches: pipelines and dynamic commands ──────────────────────────
    section("matches: pipelines and dynamic commands");
    {
        const std::string p =
            patterns_of(derive_terminal("git status && git log -n 3"));
        check(fp::matches("terminal", p,
                          "{\"command\":\"git status && git log -n 5\"}"),
              "every subcommand matches -> match");
        check(!fp::matches("terminal", p,
                           "{\"command\":\"git status && git diff\"}"),
              "second subcommand differs -> no match");
        check(!fp::matches("terminal", p, "{\"command\":\"git $x\"}"),
              "dynamic command does not parse -> no match");
    }

    // ── matches: web.fetch hosts and tool mismatch ───────────────────────
    section("matches: web.fetch and tool mismatch");
    {
        const std::string p =
            fp::serialize_patterns({"example.com"});
        check(fp::matches("web.fetch", p,
                          "{\"url\":\"https://example.com/deep/link\"}"),
              "same host, any path -> match");
        check(!fp::matches("web.fetch", p,
                           "{\"url\":\"https://other.com\"}"),
              "different host -> no match");
        check(!fp::matches("terminal", p, "{\"command\":\"ls\"}"),
              "host pattern on the wrong tool -> no match");
    }

    // ── storage round-trip ───────────────────────────────────────────────
    section("storage round-trip");
    {
        const std::vector<std::string> in = {"git status", "^/a/"};
        const std::string serialized = fp::serialize_patterns(in);
        const std::vector<std::string> out = fp::parse_patterns(serialized);
        check(out.size() == 2 && out[0] == "git status" && out[1] == "^/a/",
              "serialize/parse round-trip");
        check(fp::serialize_patterns({}) == "", "empty list -> empty string");
        check(fp::parse_patterns("").empty(), "empty string -> empty list");
        check(fp::parse_patterns("not-json").empty(),
              "garbage -> empty list (fail-closed)");
        check(fp::label_for("") == "whole tool", "label: whole tool");
        check(fp::label_for(serialized) == "git status, ^/a/",
              "label: joined patterns");
    }

    std::cout << (g_fail == 0 ? "\nALL PASS\n" : "\nFAILURES: " +
                                                 std::to_string(g_fail) + "\n");
    return g_fail;
}
