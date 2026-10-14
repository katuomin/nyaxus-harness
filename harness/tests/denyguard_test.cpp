// Deny-only monotonic guards: the guard layer in front of the approval
// waterfall (design: docs/decisions/deny-guards.md).
//
// Pure logic - no model artifacts, no daemon. The guard can only deny, never
// allow, so the matrix is: obviously-destructive argv and credential-shaped
// arguments deny with a named reason; everything else (including ordinary
// writes that the gate handles) passes through untouched.
#include <iostream>
#include <string>

#include "nyaxus/argvpolicy.hpp"
#include "nyaxus/denyguard.hpp"

using namespace nyaxus;
namespace dg = nyaxus::denyguard;
namespace ap = nyaxus::argvpolicy;
using ap::Shell;

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

// Escape a command for embedding in a JSON string literal.
// Escape a command for embedding in a JSON string literal.
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '"') {
            out.push_back('\\');
            out.push_back('"');
        } else if (c == '\\') {
            out.push_back('\\');
            out.push_back('\\');
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// terminal tool with a command; the guard's reason must name the culprit.
static void term(Shell shell, const std::string& command, bool want_deny,
                 const std::string& tag) {
    const std::string args = "{\"command\":\"" + json_escape(command) + "\"}";
    const auto verdict = dg::evaluate("terminal", args, shell);
    check(verdict.deny == want_deny,
          tag + (want_deny ? "denies: " : "passes: ") + command);
    if (want_deny) {
        check(!verdict.reason.empty(), tag + "reason names the culprit: " + command);
    }
}

int main() {
    section("destructive argv (posix)");
    term(Shell::Posix, "rm -rf /", true, "[sh] ");
    term(Shell::Posix, "rm -fr /", true, "[sh] ");
    term(Shell::Posix, "rm -rf ~", true, "[sh] ");
    term(Shell::Posix, "rm -rf ~/*", true, "[sh] ");
    term(Shell::Posix, "rm -rf /etc", true, "[sh] ");
    term(Shell::Posix, "rm -rf /", true, "[sh] ");
    term(Shell::Posix, "/bin/rm -rf /usr", true, "[sh] ");
    term(Shell::Posix, "rm -rf /home/<user>/project", false, "[sh] ");
    term(Shell::Posix, "rm -rf ./build", false, "[sh] ");
    term(Shell::Posix, "rm one.txt", false, "[sh] ");
    term(Shell::Posix, "rm -f one.txt", false, "[sh] ");
    term(Shell::Posix, "mkfs.ext4 /dev/sda1", true, "[sh] ");
    term(Shell::Posix, "mkfs /dev/nvme0n1p2", true, "[sh] ");
    term(Shell::Posix, "dd if=/dev/zero of=/dev/sda bs=1M", true, "[sh] ");
    term(Shell::Posix, "dd if=image.iso of=/dev/null", false, "[sh] ");
    term(Shell::Posix, "dd if=out.bin of=/dev/stdout", false, "[sh] ");
    term(Shell::Posix, "shutdown now", true, "[sh] ");
    term(Shell::Posix, "reboot", true, "[sh] ");
    term(Shell::Posix, "init 6", true, "[sh] ");
    term(Shell::Posix, "wipefs -a /dev/sda", true, "[sh] ");
    term(Shell::Posix, "git push --force", true, "[sh] ");
    term(Shell::Posix, "git push -f origin main", true, "[sh] ");
    term(Shell::Posix, "git push --force-with-lease", false, "[sh] ");
    term(Shell::Posix, "git push origin main", false, "[sh] ");
    term(Shell::Posix, "ls -la /", false, "[sh] ");
    term(Shell::Posix, "cat /etc/hostname", false, "[sh] ");
    term(Shell::Posix, "ls && rm -rf /", true, "[sh] ");
    term(Shell::Posix, "echo $HOME", false, "[sh] ");  // dynamic: gate, not guard

    section("destructive argv (cmd)");
    term(Shell::Cmd, "format c:", true, "[cmd] ");
    term(Shell::Cmd, "FORMAT D:", true, "[cmd] ");
    term(Shell::Cmd, "diskpart", true, "[cmd] ");
    term(Shell::Cmd, "shutdown /s /t 0", true, "[cmd] ");
    term(Shell::Cmd, "del /s /q C:\\*.*", true, "[cmd] ");
    term(Shell::Cmd, "rd /s /q build", true, "[cmd] ");
    term(Shell::Cmd, "del one.txt", false, "[cmd] ");
    term(Shell::Cmd, "dir", false, "[cmd] ");

    section("secret arguments (any tool)");
    term(Shell::Posix, "curl https://api.example -H \"Authorization: Bearer sk-abcdef1234567890\"", true, "[sh] ");
    term(Shell::Posix, "export TOKEN=sk-abcdef123456789012 && build", true, "[sh] ");
    term(Shell::Posix, "gh auth login --with-token < /dev/stdin # ghp_ABCDEF1234567890ABCDEF1234567890AB", true, "[sh] ");
    term(Shell::Posix, "aws s3 ls --profile AKIAIOSFODNN7EXAMPLE", true, "[sh] ");
    term(Shell::Posix, "echo '-----BEGIN RSA PRIVATE KEY-----'", true, "[sh] ");
    term(Shell::Posix, "ls -la", false, "[sh] ");
    term(Shell::Posix, "git commit -m \"fix: handle bearer token rotation\"", false, "[sh] ");

    // Non-terminal tools: the secret scan applies to their argument JSON.
    const auto kv = dg::evaluate("web.fetch",
                                 R"({"url":"https://x","api_key":"hunter2secret"})",
                                 Shell::Posix);
    check(kv.deny, "web.fetch denies: api_key value");
    check(!kv.reason.empty(), "web.fetch reason names the culprit");
    const auto kv_num = dg::evaluate("memory.recall",
                                     R"({"query":"the token count","limit":5})",
                                     Shell::Posix);
    check(!kv_num.deny, "memory.recall passes: no credential shape");
    const auto kv_short = dg::evaluate("terminal",
                                       R"({"command":"echo token=abc"})",
                                       Shell::Posix);
    check(!kv_short.deny, "short token value passes (not credential-shaped)");

    if (g_fail == 0) {
        std::cout << "\nDENY_GUARD_TEST_PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_fail << " FAILURES\n";
    return 1;
}
