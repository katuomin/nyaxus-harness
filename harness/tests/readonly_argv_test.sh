#!/usr/bin/env bash
# End-to-end check for the read-only argv auto-approve (docs/readonly-argv.md).
#
# The mock provider keys its behavior on the run's user message, so one
# mock process serves seven scenarios (the daemon prefers a unix /bin/sh -
# git-bash on Windows - so every command is a POSIX command):
#   A "auto approve"    - git status must skip the gate entirely: no
#     approval.requested, and exactly one approval.auto (reason
#     readonly-argv) in the ledger.
#   B "gate write"      - rm x.txt must still gate: approval.requested,
#     no approval.auto.
#   C "redirect file"   - ls > out.txt gates (output redirect to a file).
#   D "redirect nul"    - ls > /dev/null auto-approves (null device).
#   E "sequence ok"     - ls && git status auto-approves (all stages
#     read-only).
#   F "sequence mixed"  - ls && rm x gates (one write stage).
#   G "dynamic gates"   - ls $HOME gates ($ expansion is dynamic).
#
# Usage: harness/tests/readonly_argv_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8929
DAEMON_PORT=8930
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_argv_policy_provider.mjs" > mock.log 2>&1 &
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
# collect the run's approval events, and locate the final assistant message.
ASSERT_PROLOGUE='
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const run = process.argv[1];
    const reqs = (snap.events || []).filter((e) => e.event_type === "approval.requested" && e.run_id === run);
    const autos = (snap.events || []).filter((e) => e.event_type === "approval.auto" && e.run_id === run);
    const a = snap.messages.find((m) => m.run_id === run && m.role === "assistant");
    if (!a) fail("no assistant message");
    if (!a.is_active) fail("assistant message not active");
'

run_auto() {  # $1 label  $2 scenario message  $3 command regex  $4 answer regex
    local sid run
    read -r sid run < <(run_scenario "$2")
    NODE_PROG="$ASSERT_PROLOGUE"'
    if (reqs.length !== 0) fail("expected no approval.requested, got " + reqs.length);
    if (autos.length !== 1) fail("expected exactly one approval.auto, got " + autos.length);
    if (autos[0].payload.rule_id !== "readonly-argv") fail("approval.auto rule is not readonly-argv: " + autos[0].payload.rule_id);
    if (!new RegExp(process.argv[3]).test(autos[0].payload.arguments)) fail("approval.auto arguments missing the command: " + autos[0].payload.arguments);
    if (!new RegExp(process.argv[4]).test(a.content)) fail("final answer is not the model answer: " + a.content);
    console.log(process.argv[2] + ": auto-approved, no gate");
  });
'
    curl -sf "$BASE/sessions/$sid" | "$NODE" -e "$NODE_PROG" "$run" "$1" "$3" "$4" || fail "scenario $1 assertions failed"
}

run_gate() {  # $1 label  $2 scenario message  $3 answer regex
    local sid run
    read -r sid run < <(run_scenario "$2")
    NODE_PROG="$ASSERT_PROLOGUE"'
    if (reqs.length !== 1) fail("expected exactly one approval.requested, got " + reqs.length);
    if (autos.length !== 0) fail("expected no approval.auto, got " + autos.length);
    if (!new RegExp(process.argv[3]).test(a.content)) fail("final answer is not the model answer: " + a.content);
    console.log(process.argv[2] + ": gated as expected");
  });
'
    curl -sf "$BASE/sessions/$sid" | "$NODE" -e "$NODE_PROG" "$run" "$1" "$3" || fail "scenario $1 assertions failed"
}

echo "== A: read-only call skips the gate =="
run_auto "A" "auto approve" "git status" "auto approve done"

echo "== B: write call still gates =="
run_gate "B" "gate write" "gate write done"

echo "== C: output redirect to a file gates =="
run_gate "C" "redirect file" "redirect file done"

echo "== D: output redirect to /dev/null auto-approves =="
run_auto "D" "redirect nul" "ls > /dev/null" "redirect nul done"

echo "== E: all-read-only sequence auto-approves =="
run_auto "E" "sequence ok" "ls && git status" "sequence ok done"

echo "== F: mixed sequence gates =="
run_gate "F" "sequence mixed" "sequence mixed done"

echo "== G: dynamic word gates =="
run_gate "G" "dynamic gates" "dynamic gates done"

echo
echo "READONLY_ARGV_TEST_PASSED"
