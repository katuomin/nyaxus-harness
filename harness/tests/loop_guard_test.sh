#!/usr/bin/env bash
# End-to-end check for the deterministic loop guards (docs/loop-guards.md).
#
# The mock provider keys its behavior on the run's user message, so one
# mock process serves four scenarios:
#   A "check the build" - an inert spin (echo ok x3); the 3rd call is
#     suppressed with the inert-loop guard text and the mock pivots on it.
#     Asserts the suppress event (inert=true), the stub result, and the
#     model's pivot as the final answer.
#   B "loop forever" - the same git status call, ignoring the guard text;
#     the daemon must give up with a synthesized narrative (the run
#     completes, provider calls bounded at 4, well under the 20-iteration
#     budget).
#   C "steady work" - three distinct non-inert commands, then an answer;
#     no loop.guard event may fire at all.
#   D "twice only" - git status twice; the 2nd result carries the
#     change-approach note, and nothing is suppressed.
#
# Usage: harness/tests/loop_guard_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8927
DAEMON_PORT=8928
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_loop_guard_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

wait_health() {
    for _ in $(seq 1 50); do
        if curl -sf "$BASE/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
wait_health "$BASE" || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

# Runs one scenario to completion: creates a session, starts a run with the
# given message, auto-approves terminal calls (session scope) while polling,
# and echoes "session-id run-id". Fails the test on run.failed/run.cancelled.
run_scenario() {
    local message="$1"
    local sid run snap aid done=0
    sid="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "no session id"
    run="$(curl -sf -X POST "$BASE/sessions/$sid/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$sid\",\"message\":\"$message\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run rejected"
    for _ in $(seq 1 600); do
        snap="$(curl -sf "$BASE/sessions/$sid" 2>/dev/null || true)"
        aid="$(printf '%s' "$snap" | "$NODE" -e '
      let raw = "";
      process.stdin.on("data", (c) => (raw += c));
      process.stdin.on("end", () => {
        try {
          const snap = JSON.parse(raw);
          const ev = [...(snap.events || [])].reverse().find((e) => e.event_type === "approval.requested" && e.run_id === process.argv[1]);
          console.log(ev ? (ev.payload.approval_id || "") : "");
        } catch { console.log(""); }
      });
    ' "$run")"
        if [ -n "$aid" ]; then
            curl -sf -X POST "$BASE/runs/$run/commands" -H 'Content-Type: application/json' \
                -d "{\"command\":\"approve\",\"scope\":\"session\",\"approval_id\":\"$aid\"}" > /dev/null
        fi
        if printf '%s' "$snap" | grep -q '"event_type":"run.completed"'; then
            done=1
            break
        fi
        if printf '%s' "$snap" | grep -q '"event_type":"run.failed"' || printf '%s' "$snap" | grep -q '"event_type":"run.cancelled"'; then
            fail "run did not complete: $(printf '%s' "$snap" | grep -o '"event_type":"run\.\(failed\|cancelled\)"[^}]*')"
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run did not complete in time"
    echo "$sid $run"
}

# Shared assertion prologue: parse the session snapshot, bind snap/run/fail,
# collect the run's loop.guard events, and locate the final assistant message.
ASSERT_PROLOGUE='
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const run = process.argv[1];
    const guards = (snap.events || []).filter((e) => e.event_type === "loop.guard" && e.run_id === run);
    const a = snap.messages.find((m) => m.run_id === run && m.role === "assistant");
    if (!a) fail("no assistant message");
    if (!a.is_active) fail("assistant message not active");
'

echo "== A: inert spin is suppressed, the model pivots =="
read -r SID_A RUN_A < <(run_scenario "check the build")
NODE_PROG="$ASSERT_PROLOGUE"'
    const suppress = guards.find((e) => e.payload.stage === "suppress" && e.payload.inert === "true");
    if (!suppress) fail("no inert suppress loop.guard event");
    if (!/pivoted to real work/.test(a.content)) {
      fail("final answer is not the model pivot: " + a.content);
    }
    const stub = (snap.events || []).find((e) => e.event_type === "tool.completed"
        && e.run_id === run
        && (e.payload.result || "").includes("[inert-loop guard]"));
    if (!stub) fail("no tool.completed carries the inert guard text");
    console.log("A: inert spin suppressed (inert=true), model pivoted");
  });
'
curl -sf "$BASE/sessions/$SID_A" | "$NODE" -e "$NODE_PROG" "$RUN_A" || fail "scenario A assertions failed"

echo "== B: identical repeat, guard ignored -> give-up narrative =="
read -r SID_B RUN_B < <(run_scenario "loop forever")
NODE_PROG="$ASSERT_PROLOGUE"'
    const giveUp = guards.find((e) => e.payload.stage === "give_up");
    if (!giveUp) fail("no give_up loop.guard event");
    if (giveUp.payload.inert !== "false") fail("give_up should be the repeat variant");
    if (!/stopped here to avoid spinning/.test(a.content)) {
      fail("final message is not the give-up narrative: " + a.content);
    }
    const calls = (snap.events || []).filter((e) => e.event_type === "metrics.snapshot" && e.run_id === run).length;
    if (calls !== 4) fail("expected exactly 4 provider calls, got " + calls);
    console.log("B: give-up narrative on the 2nd fully-suppressed batch (4 provider calls)");
  });
'
curl -sf "$BASE/sessions/$SID_B" | "$NODE" -e "$NODE_PROG" "$RUN_B" || fail "scenario B assertions failed"

echo "== C: distinct real work fires no guard =="
read -r SID_C RUN_C < <(run_scenario "steady work")
NODE_PROG="$ASSERT_PROLOGUE"'
    if (guards.length !== 0) fail("expected no loop.guard events, got " + guards.length);
    if (!/steady work done/.test(a.content)) fail("final answer is not the model answer: " + a.content);
    console.log("C: no loop.guard events for distinct real work");
  });
'
curl -sf "$BASE/sessions/$SID_C" | "$NODE" -e "$NODE_PROG" "$RUN_C" || fail "scenario C assertions failed"

echo "== D: 2nd identical call gets the note, nothing suppressed =="
read -r SID_D RUN_D < <(run_scenario "twice only")
NODE_PROG="$ASSERT_PROLOGUE"'
    const reminder = guards.find((e) => e.payload.stage === "reminder");
    if (!reminder) fail("no reminder loop.guard event");
    if (guards.find((e) => e.payload.stage === "suppress")) fail("nothing should be suppressed");
    const withNote = (snap.events || []).find((e) => e.event_type === "tool.completed"
        && e.run_id === run
        && (e.payload.result || "").includes("[repeat note]"));
    if (!withNote) fail("no tool.completed carries the repeat note");
    if (!/noted the repeat/.test(a.content)) fail("final answer is not the model answer: " + a.content);
    console.log("D: 2nd identical call ran with the change-approach note");
  });
'
curl -sf "$BASE/sessions/$SID_D" | "$NODE" -e "$NODE_PROG" "$RUN_D" || fail "scenario D assertions failed"

echo
echo "LOOP_GUARD_TEST_PASSED"
