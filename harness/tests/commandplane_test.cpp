// Command plane: the pure drain evaluator for the durable command queue
// (design: docs/command-plane.md).
//
// evaluate_command is the normative drain spec: processed-id dedupe ->
// Skip (idempotent re-delivery, restart replay), TTL -> Expired,
// newer-interrupt-supersedes-older -> Superseded, interrupt-on-past-turn
// -> Superseded, else Execute. Pure: same inputs, same verdict, no I/O.
#include <iostream>
#include <string>
#include <vector>

#include "nyaxus/commandplane.hpp"

using namespace nyaxus;
namespace cp = nyaxus::commandplane;

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

static cp::State state(long long now = 1000, long long turn = 0,
                       std::vector<std::string> processed = {}) {
    cp::State s;
    s.now_ms = now;
    s.current_turn = turn;
    s.processed_ids = std::move(processed);
    return s;
}

static cp::Command steer(const std::string& id = "s1", long long issued = 990,
                         long long ttl = 0) {
    cp::Command c;
    c.id = id;
    c.kind = cp::Kind::Steer;
    c.issued_at_ms = issued;
    c.ttl_ms = ttl;
    return c;
}

static cp::Command interrupt(const std::string& id, long long issued,
                             long long target_turn) {
    cp::Command c;
    c.id = id;
    c.kind = cp::Kind::Interrupt;
    c.issued_at_ms = issued;
    c.target_turn = target_turn;
    return c;
}

int main() {
    // ── Execute: the default ─────────────────────────────────────────────
    section("execute: fresh commands");
    {
        check(cp::evaluate(steer(), state()) == cp::Verdict::Execute,
              "fresh steer executes");
        cp::Command run;
        run.id = "r1";
        run.kind = cp::Kind::Run;
        check(cp::evaluate(run, state()) == cp::Verdict::Execute,
              "fresh run executes");
        // No id (legacy commands) and no TTL: always the execute path.
        cp::Command legacy;
        legacy.kind = cp::Kind::Steer;
        check(cp::evaluate(legacy, state(10'000'000, 5, {"a", "b"})) ==
              cp::Verdict::Execute,
              "idless legacy command executes (no dedupe, no TTL)");
    }

    // ── Skip: processed-id dedupe ────────────────────────────────────────
    section("skip: processed-id dedupe (idempotent re-delivery)");
    {
        check(cp::evaluate(steer("s1"), state(1000, 0, {"s1"})) ==
              cp::Verdict::Skip,
              "re-delivered id skips");
        check(cp::evaluate(steer("s2"), state(1000, 0, {"s1"})) ==
              cp::Verdict::Execute,
              "a different id is not affected");
        // Dedupe wins over TTL: an already-processed entry that is now
        // stale is a re-delivery, not a new expiry (no double outcome).
        check(cp::evaluate(steer("s1", 0, 100), state(1000, 0, {"s1"})) ==
              cp::Verdict::Skip,
              "dedupe takes precedence over expiry");
    }

    // ── Expired: TTL ─────────────────────────────────────────────────────
    section("expired: TTL");
    {
        check(cp::evaluate(steer("s1", 900, 50), state(1000)) ==
              cp::Verdict::Expired,
              "past the TTL expires (1000 - 900 > 50)");
        check(cp::evaluate(steer("s1", 950, 50), state(1000)) ==
              cp::Verdict::Execute,
              "at exactly the TTL edge it still executes (not >)");
        check(cp::evaluate(steer("s1", 900, 0), state(1000)) ==
              cp::Verdict::Execute,
              "ttl 0 = no TTL");
    }

    // ── Superseded: newer interrupt for the same target ──────────────────
    section("superseded: newer interrupt wins");
    {
        // Live turn == target turn (2), so the past-turn rule stays out of
        // the way and only the supersession rule is under test.
        const cp::Command older = interrupt("i1", 900, 2);
        const cp::Command newer = interrupt("i2", 950, 2);
        check(cp::evaluate(older, state(1000, 2), {newer}) ==
              cp::Verdict::Superseded,
              "older interrupt superseded by a newer one");
        check(cp::evaluate(newer, state(1000, 2), {older}) ==
              cp::Verdict::Execute,
              "the newer interrupt still executes");
        // Same target required: a newer interrupt for a different turn
        // does not supersede.
        const cp::Command other = interrupt("i3", 990, 3);
        check(cp::evaluate(older, state(1000, 2), {other}) ==
              cp::Verdict::Execute,
              "different target turn: no supersession");
        // Exact-tie issue time: the lexicographically greater id wins, and
        // the same id never supersedes itself (redelivery stays a Skip).
        const cp::Command a = interrupt("a", 900, 2);
        const cp::Command b = interrupt("b", 900, 2);
        check(cp::evaluate(a, state(1000, 2), {b}) == cp::Verdict::Superseded,
              "tie: greater id supersedes");
        check(cp::evaluate(a, state(1000, 2, {"a"}), {b}) == cp::Verdict::Skip,
              "redelivery of a processed id stays a Skip");
        // Non-interrupt kinds ignore the pending set.
        check(cp::evaluate(steer("s1", 800), state(1000, 2), {newer}) ==
              cp::Verdict::Execute,
              "steers are not superseded by interrupts");
    }

    // ── Superseded: interrupt on a past turn ─────────────────────────────
    section("superseded: interrupt on a past turn");
    {
        check(cp::evaluate(interrupt("i1", 900, 1), state(1000, 3)) ==
              cp::Verdict::Superseded,
              "target turn 1 < live turn 3: superseded");
        check(cp::evaluate(interrupt("i1", 900, 3), state(1000, 3)) ==
              cp::Verdict::Execute,
              "target turn == live turn: still actionable");
        check(cp::evaluate(interrupt("i1", 900, -1), state(1000, 3)) ==
              cp::Verdict::Execute,
              "target -1 (current): always actionable");
    }

    // ── determinism: the same inputs always give the same verdict ────────
    section("determinism");
    {
        const cp::Command c = steer("s1", 900, 50);
        const cp::State s = state(1000, 0, {"x"});
        bool same = true;
        for (int i = 0; i < 100; ++i) {
            if (cp::evaluate(c, s, {}) != cp::Verdict::Expired) same = false;
        }
        check(same, "100 evaluations, one verdict");
    }

    // ── names ────────────────────────────────────────────────────────────
    section("names");
    {
        check(std::string(cp::kind_name(cp::Kind::Steer)) == "steer" &&
              std::string(cp::kind_name(cp::Kind::Interrupt)) == "interrupt",
              "kind names");
        check(std::string(cp::verdict_name(cp::Verdict::Superseded)) ==
              "superseded",
              "verdict names");
    }

    std::cout << "\n" << (g_fail == 0 ? "COMMANDPLANE_TEST_PASSED"
                                      : "COMMANDPLANE_TEST_FAILED")
              << " (" << g_fail << " failures)\n";
    return g_fail == 0 ? 0 : 1;
}
