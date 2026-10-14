// Loop guard: inert-command classification, canonical args, and the
// sliding-window repeat verdicts (design: docs/loop-guards.md).
//
// Pure logic - no model artifacts, no daemon. The inert corpus includes the
// verbatim commands from the #808 report (the run no existing guard could
// see).
#include <iostream>
#include <string>
#include <vector>

#include "nyaxus/loopguard.hpp"

using namespace nyaxus;
namespace lg = nyaxus::loopguard;

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

static void inert_cases(const std::vector<const char*>& inert,
                        const std::vector<const char*>& not_inert) {
    for (const char* cmd : inert) {
        check(lg::is_inert_command(cmd), std::string("inert: ") + cmd);
    }
    for (const char* cmd : not_inert) {
        check(!lg::is_inert_command(cmd), std::string("not inert: ") + cmd);
    }
}

int main() {
    section("inert: the #808 spin (verbatim from the report)");
    inert_cases(
        {"echo \"ready\"",
         "echo done",
         "echo ok",
         "true",
         "echo \"switching to memory tool\"",
         "echo \"final check before memory call\""},
        {});

    section("inert: null builtins and sequences of them");
    inert_cases({":", "false", "true; true", "echo a && true",
                 "echo a\necho b\n", "  echo spaced  ;  ", "printf 'hello'"},
                {});

    section("inert: real work is never inert");
    inert_cases(
        {},
        {"cargo test",
         "git status",
         "ls",
         "pwd",
         "date",
         "cat src/main.rs",
         // Anything reaching outward, even wrapped around an echo.
         "echo hi > file.txt",
         "echo $HOME",
         "echo `hostname`",
         "echo hi | wc -l",
         "echo hi &",
         "(echo hi)",
         "python3 - << 'PYEOF'\nprint(\"x\")\nPYEOF",
         // This one genuinely attempted something.
         "cd /home/x && memory_replace 2>/dev/null || echo \"no shell "
         "memory tool\"",
         // A real command anywhere in the sequence disqualifies it.
         "echo starting && cargo build",
         "true; rm -rf build"});

    section("inert: quote-blind splitting only ever errs toward not-inert");
    inert_cases({}, {"echo \"a; true\"", "echo 'x && true'"});

    section("inert: an empty command is not inert (malformed, a different "
            "problem)");
    inert_cases({}, {"", "   \n  ", ";;"});

    section("terminal-call classification");
    check(lg::is_inert_terminal_call("terminal",
                                     "{\"command\":\"echo ok\"}"),
          "terminal echo ok is inert");
    check(!lg::is_inert_terminal_call("terminal",
                                      "{\"command\":\"cargo test\"}"),
          "terminal cargo test is not inert");
    check(!lg::is_inert_terminal_call("shell_plugin",
                                      "{\"command\":\"echo ok\"}"),
          "same arg shape, different tool: not our judgement");
    check(!lg::is_inert_terminal_call("terminal", "{\"name\":\"x\"}"),
          "missing command field is not inert");
    check(!lg::is_inert_terminal_call("terminal", "{}"),
          "empty args are not inert");
    check(!lg::is_inert_terminal_call("terminal", "not json"),
          "malformed args are not inert");
    check(!lg::is_inert_terminal_call("terminal",
                                      "{\"command\":\"\"}"),
          "empty command is not inert");

    section("canonical args: key order cannot distinguish identical calls");
    check(lg::canonical_args("{\"command\":\"echo ok\",\"a\":1}") ==
              lg::canonical_args("{\"a\":1,\"command\":\"echo ok\"}"),
          "key order is normalized");
    check(lg::canonical_args("not json") == "not json",
          "parse failure keeps the raw string");
    check(lg::canonical_args("\"a string\"") == "\"a string\"",
          "non-object values keep the raw string");
    check(lg::canonical_args("{\"b\":2,\"a\":1}") ==
              lg::canonical_args("{\"a\":1,\"b\":2}"),
          "objects are key-sorted");

    section("guard: identical repeats (threshold 3)");
    {
        lg::Guard guard;
        const std::string args = "{\"command\":\"git status\"}";
        check(guard.inspect("terminal", args).kind ==
                  lg::Verdict::Kind::Pass,
              "1st identical call passes");
        check(guard.inspect("terminal", args).kind ==
                  lg::Verdict::Kind::Reminder,
              "2nd identical call gets the change-approach reminder");
        auto third = guard.inspect("terminal", args);
        check(third.kind == lg::Verdict::Kind::Suppress,
              "3rd identical call is suppressed");
        check(!third.inert, "git status is not inert");
        check(guard.inspect("terminal", args).kind ==
                  lg::Verdict::Kind::Suppress,
              "suppressed calls are not recorded; the 4th is suppressed "
              "too");
        // A different call in between is a different signature.
        check(guard.inspect("terminal", "{\"command\":\"ls\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "a different command is a different signature");
    }

    section("guard: inert spin collapses onto one signature");
    {
        lg::Guard guard;
        auto v1 = guard.inspect("terminal", "{\"command\":\"echo ready\"}");
        auto v2 = guard.inspect("terminal", "{\"command\":\"echo ok\"}");
        auto v3 = guard.inspect("terminal", "{\"command\":\"true\"}");
        check(v1.kind == lg::Verdict::Kind::Pass, "1st inert command passes");
        check(v2.kind == lg::Verdict::Kind::Reminder,
              "2nd inert command (different literal) is the 2nd occurrence");
        check(v3.kind == lg::Verdict::Kind::Suppress && v3.inert,
              "3rd inert command (different literal) is suppressed, "
              "inert=true");
    }

    section("guard: mutating calls clear inert entries");
    {
        lg::Guard guard;
        check(guard.inspect("terminal", "{\"command\":\"echo ok\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "inert echo recorded");
        check(guard.inspect("terminal", "{\"command\":\"git status\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "mutating call passes and clears the inert entry");
        check(guard.inspect("terminal", "{\"command\":\"echo ok\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "post-mutation echo starts a fresh count");
        check(guard.inspect("terminal", "{\"command\":\"echo ok\"}").kind ==
                  lg::Verdict::Kind::Reminder,
              "2nd post-mutation echo is a reminder");
        check(guard.inspect("terminal", "{\"command\":\"echo ok\"}").kind ==
                  lg::Verdict::Kind::Suppress,
              "3rd post-mutation echo is suppressed");
    }

    section("guard: inert commands do not clear the window");
    {
        lg::Guard guard;
        check(guard.inspect("terminal", "{\"command\":\"git status\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "mutating call recorded");
        check(guard.inspect("terminal", "{\"command\":\"echo ok\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "inert echo does not clear the mutating entry");
        check(guard.inspect("terminal", "{\"command\":\"git status\"}").kind ==
                  lg::Verdict::Kind::Reminder,
              "the echo did not launder the read out of reach: 2nd git "
              "status is a reminder");
    }

    section("guard: mutating calls count amongst themselves");
    {
        lg::Guard guard;
        check(guard.inspect("terminal", "{\"command\":\"git status\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "1st");
        check(guard.inspect("terminal", "{\"command\":\"ls\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "different mutating call passes");
        check(guard.inspect("terminal", "{\"command\":\"git status\"}").kind ==
                  lg::Verdict::Kind::Reminder,
              "2nd git status is a reminder");
        check(guard.inspect("terminal", "{\"command\":\"git status\"}").kind ==
                  lg::Verdict::Kind::Suppress,
              "3rd git status is suppressed");
    }

    section("guard: window trimming");
    {
        lg::Guard guard;
        guard.window_size = 2;
        check(guard.inspect("terminal", "{\"command\":\"a\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "a recorded (window [a])");
        check(guard.inspect("terminal", "{\"command\":\"b\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "b recorded (window [a,b])");
        check(guard.inspect("terminal", "{\"command\":\"c\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "c evicts a (window [b,c])");
        check(guard.inspect("terminal", "{\"command\":\"a\"}").kind ==
                  lg::Verdict::Kind::Pass,
              "a is out of the window: fresh count");
        // A call that stayed inside the window is still counted.
        check(guard.inspect("terminal", "{\"command\":\"c\"}").kind ==
                  lg::Verdict::Kind::Reminder,
              "c is still in the window (2nd occurrence)");
    }

    section("guard: reset and cross-tool isolation");
    {
        lg::Guard guard;
        const std::string args = "{\"command\":\"echo ok\"}";
        guard.inspect("terminal", args);
        guard.inspect("terminal", args);
        guard.reset();
        check(guard.inspect("terminal", args).kind == lg::Verdict::Kind::Pass,
              "reset clears the window");
        // Same canonical args, different tool: a different signature.
        guard.inspect("prompt.edit", args);
        check(guard.inspect("terminal", args).kind ==
                  lg::Verdict::Kind::Pass,
              "prompt.edit does not count toward terminal's window");
    }

    section("runaway: identical-result repeat (threshold 3)");
    {
        lg::RunawayGuard guard;
        const std::string args = "{\"query\":\"lighthouse password\"}";
        check(guard.observe("memory.recall", args, "no memories matched")
                      .kind == lg::RunawayKind::None,
              "1st identical result: no signal");
        check(guard.observe("memory.recall", args, "no memories matched")
                      .kind == lg::RunawayKind::None,
              "2nd identical result: no signal");
        auto third =
            guard.observe("memory.recall", args, "no memories matched");
        check(third.kind == lg::RunawayKind::ResultRepeat,
              "3rd identical call+result: result-repeat signal");
        check(third.tool == "memory.recall", "signal names the tool");
        // The crossing is consumed: a fresh identical triple re-arms.
        check(guard.observe("memory.recall", args, "no memories matched")
                      .kind == lg::RunawayKind::None,
              "post-signal observation starts a fresh streak");
        // A changed result is progress: it breaks the streak.
        guard.observe("memory.recall", args, "no memories matched");
        check(guard.observe("memory.recall", args, "found: seagull-42")
                      .kind == lg::RunawayKind::None,
              "a new result breaks the repeat streak");
    }

    section("runaway: polling (volatile digits, same result)");
    {
        lg::RunawayGuard guard;
        check(guard.observe("terminal",
                            "{\"command\":\"curl status?poll=1\"}",
                            "building: 20%")
                      .kind == lg::RunawayKind::None,
              "1st poll: no signal");
        check(guard.observe("terminal",
                            "{\"command\":\"curl status?poll=2\"}",
                            "building: 20%")
                      .kind == lg::RunawayKind::None,
              "2nd poll (different digit): no signal");
        auto third = guard.observe("terminal",
                                   "{\"command\":\"curl status?poll=3\"}",
                                   "building: 20%");
        check(third.kind == lg::RunawayKind::Polling,
              "3rd near-identical poll with the same result: polling");
        // A changing result is legitimate progress, not a loop.
        guard.observe("terminal",
                      "{\"command\":\"curl status?poll=4\"}", "building: 40%");
        guard.observe("terminal",
                      "{\"command\":\"curl status?poll=5\"}", "building: 60%");
        check(guard.observe("terminal",
                            "{\"command\":\"curl status?poll=6\"}",
                            "building: 80%")
                      .kind == lg::RunawayKind::None,
              "changing results are progress, not a loop");
    }

    section("runaway: same-error-family repeat");
    {
        lg::RunawayGuard guard;
        const std::string e1 = "git: command failed: remote error: "
                               "refused to accept ref (line 42)";
        const std::string e2 = "git: command failed: remote error: "
                               "refused to accept ref (line 97)";
        check(guard.observe("terminal", "{\"command\":\"git push\"}", e1)
                      .kind == lg::RunawayKind::None,
              "1st error: no signal");
        // Different args (the model tweaked the call), same normalized
        // error family (digits masked).
        check(guard.observe("terminal", "{\"command\":\"git push origin\"}", e2)
                      .kind == lg::RunawayKind::None,
              "2nd same-family error: no signal");
        auto third = guard.observe("terminal",
                                   "{\"command\":\"git push origin main\"}", e1);
        check(third.kind == lg::RunawayKind::ErrorRepeat,
              "3rd same-family error: error-repeat signal");
        // A successful result between errors is not an error.
        guard.observe("terminal", "{\"command\":\"ls\"}", "a.txt b.txt");
        guard.observe("terminal", "{\"command\":\"git push\"}", e1);
        check(guard.observe("terminal", "{\"command\":\"git push origin\"}", e2)
                      .kind == lg::RunawayKind::None,
              "a success in between breaks the error streak");
    }

    section("runaway: A-B-A-B alternation");
    {
        lg::RunawayGuard guard;
        const std::string a = "{\"command\":\"cat a.txt\"}";
        const std::string b = "{\"command\":\"cat b.txt\"}";
        check(guard.observe("terminal", a, "contents of a").kind ==
                      lg::RunawayKind::None,
              "A: no signal");
        check(guard.observe("terminal", b, "contents of b").kind ==
                      lg::RunawayKind::None,
              "B: no signal");
        check(guard.observe("terminal", a, "contents of a").kind ==
                      lg::RunawayKind::None,
              "A again: no signal (2nd A, 2nd B not both present)");
        auto fourth = guard.observe("terminal", b, "contents of b");
        check(fourth.kind == lg::RunawayKind::Alternation,
              "A-B-A-B: alternation signal");
        // A-B-A-C is not an alternation.
        guard.observe("terminal", a, "contents of a");
        guard.observe("terminal", b, "contents of b");
        guard.observe("terminal", a, "contents of a");
        check(guard.observe("terminal",
                            "{\"command\":\"cat c.txt\"}", "contents of c")
                      .kind == lg::RunawayKind::None,
              "A-B-A-C is not an alternation");
    }

    section("runaway: cross-tool isolation and note");
    {
        lg::RunawayGuard guard;
        guard.observe("terminal", "{\"command\":\"ls\"}", "a.txt");
        guard.observe("web.fetch", "{\"url\":\"http://x\"}", "a.txt");
        check(guard.observe("terminal", "{\"command\":\"ls\"}", "a.txt")
                      .kind == lg::RunawayKind::None,
              "different tools with the same result do not share a streak");
        const std::string note = lg::runaway_note(lg::RunawayKind::Polling);
        check(note.find("[loop reminder]") == 0, "note names itself");
        check(note.find("near-identical") != std::string::npos,
              "polling note names the shape");
        check(std::string(lg::kind_name(lg::RunawayKind::ResultRepeat)) ==
                  "result-repeat",
              "kind names for the ledger");
    }

    section("guard texts");
    check(lg::repeat_guard_text().find("[repeat-loop guard]") !=
              std::string::npos,
          "repeat guard names itself");
    check(lg::inert_guard_text().find("[inert-loop guard]") !=
              std::string::npos,
          "inert guard names itself");
    check(lg::give_up_text(false).find("stopped here to avoid spinning") !=
              std::string::npos,
          "repeat give-up narrative");
    check(lg::give_up_text(true).find("spinning") != std::string::npos,
          "inert give-up narrative");
    check(lg::kRepeatNote != nullptr &&
          std::string(lg::kRepeatNote).find("[repeat note]") == 0,
          "repeat note names itself");

    if (g_fail == 0) {
        std::cout << "\nLOOPGUARD_TEST_PASSED\n";
        return 0;
    }
    std::cout << "\n" << g_fail << " FAILURES\n";
    return 1;
}
