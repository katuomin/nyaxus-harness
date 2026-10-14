// Structured permission decisions: the risk/side-effect taxonomy attached
// to every gate decision (manual approve/deny, readonly-argv auto-approve,
// always-allow bypass) so the ledger records *why* a call ran.
//
// Pure logic - no model artifacts, no daemon.
#include <iostream>
#include <string>

#include "nyaxus/gatepolicy.hpp"

using namespace nyaxus;
namespace gp = nyaxus::gatepolicy;

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

int main() {
    section("read-only calls");
    {
        const gp::CallRisk risk =
            gp::classify("memory.recall", "{\"query\":\"x\"}", true, true);
        check(risk.risk_level == "low" && risk.side_effect_scope == "none",
              "read-only manifest tool -> low/none");
    }
    {
        const gp::CallRisk risk =
            gp::classify("terminal", "{\"command\":\"git status\"}", true, true);
        check(risk.risk_level == "low" && risk.side_effect_scope == "none",
              "argv-proven read-only terminal -> low/none");
    }

    section("workspace writes");
    {
        const gp::CallRisk risk =
            gp::classify("terminal", "{\"command\":\"git commit -m x\"}", false, true);
        check(risk.risk_level == "medium" && risk.side_effect_scope == "workspace",
              "unix terminal write -> medium/workspace");
    }
    {
        const gp::CallRisk risk =
            gp::classify("memory.save", "{\"text\":\"fact\"}", false, true);
        check(risk.risk_level == "medium" && risk.side_effect_scope == "workspace",
              "memory write -> medium/workspace");
    }

    section("harness-state writes");
    {
        const gp::CallRisk risk =
            gp::classify("prompt.edit", "{\"path\":\"nyaxus/CORE.md\"}", false, true);
        check(risk.risk_level == "system" && risk.side_effect_scope == "system",
              "prompt.edit -> system/system");
    }

    section("cmd-shell fallback");
    {
        const gp::CallRisk risk =
            gp::classify("terminal", "{\"command\":\"del x.txt\"}", false, false);
        check(risk.risk_level == "system" && risk.side_effect_scope == "system",
              "cmd.exe write is not provably workspace-scoped -> system");
    }

    std::cout << (g_fail == 0 ? "\nALL PASS\n" : "\nFAILURES: " +
                                                 std::to_string(g_fail) + "\n");
    return g_fail;
}
