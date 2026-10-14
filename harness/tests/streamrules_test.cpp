// Stream rules: dormant regex rules over the streamed assistant text.
// Pure logic - no model artifacts, no daemon.
#include <iostream>
#include <string>
#include <vector>

#include "nyaxus/streamrules.hpp"

using namespace nyaxus;
namespace sr = nyaxus::streamrules;

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
    section("work-claim: fires on a completion claim with no tools");
    {
        sr::Rules rules;
        auto fired = rules.observe("All fixed and done.", false);
        check(fired.size() == 1 && fired[0].id == "work-claim",
              "claim without tools fires work-claim");
        check(fired.empty() || fired[0].reminder.find("[stream rule]") == 0,
              "reminder names itself");
    }

    section("work-claim: case-insensitive, word-boundary");
    {
        sr::Rules rules;
        check(!rules.observe("The DONE deal is interesting.", false).empty(),
              "uppercase marker fires");
        sr::Rules rules2;
        check(rules2.observe("undone work remains", false).empty(),
              "no word-boundary match on 'undone'");
    }

    section("work-claim: a tool has run -> grounded, quiet");
    {
        sr::Rules rules;
        check(rules.observe("done.", true).empty(),
              "claim after tool work does not fire");
    }

    section("dedup: one reminder per rule per run");
    {
        sr::Rules rules;
        rules.observe("done.", false);
        check(rules.observe("finished.", false).empty(),
              "the rule stays fired for the rest of the run");
        rules.reset();
        check(!rules.observe("done.", false).empty(),
              "reset re-arms the rule (next run)");
    }

    section("narrated-intent: fires on described actions, not claims");
    {
        sr::Rules rules;
        auto fired = rules.observe("Let me check the build log.", false);
        check(fired.size() == 1 && fired[0].id == "narrated-intent",
              "'let me' fires narrated-intent");
        sr::Rules rules2;
        auto fired2 = rules2.observe("I will now run the tests.", false);
        check(fired2.size() == 1 && fired2[0].id == "narrated-intent",
              "'i will now' fires narrated-intent");
    }

    section("narrated-intent: ordinary prose does not fire");
    {
        sr::Rules rules;
        check(rules.observe("The build passed; the answer is 42.", false)
                      .empty(),
              "plain prose is quiet");
        sr::Rules rules2;
        check(rules2.observe("let me know if you need more", false).empty(),
              "'let me know' is not an intent to act");
    }

    section("independent rules: both can fire in one run");
    {
        sr::Rules rules;
        auto fired = rules.observe("Let me fix this - done.", false);
        check(fired.size() == 2,
              "a text with both markers fires both rules");
    }

    section("empty text is quiet");
    {
        sr::Rules rules;
        check(rules.observe("", false).empty(), "empty text: no signal");
    }

    if (g_fail == 0) {
        std::cout << "\nSTREAMRULES_TEST_PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_fail << " FAILURES\n";
    return 1;
}
