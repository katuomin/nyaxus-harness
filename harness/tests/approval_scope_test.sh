#!/usr/bin/env bash
# End-to-end check for session-scoped tool approval against the daemon.
#
# A mock OpenAI-compatible provider calls the "terminal" tool four times, then
# answers with plain text: "rm one.txt", "rm one.txt", "rm two.txt",
# "cat big.txt". The operator approves the first call with "allow session"
# scope. "Allow for session" approves the tool itself, so every later terminal
# call (with any arguments) must run without a fresh prompt. "cat big.txt"
# reads a 5 MiB file, so its output must be truncated at the 256 KiB
# tool-output cap. (The rm calls are classifier-gated writes, so the first
# call really does prompt: read-only commands now auto-approve.)
#
# Asserts on the event ledger:
#   - exactly one approval.requested (only the first call prompts)
#   - four tool.completed (all tool calls executed)
#   - the big.txt result carries the "[...] bytes of output truncated [...]" note
#   - one run.completed (the run did not hang on an approval)
#
# Usage: harness/tests/approval_scope_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8897
DAEMON_PORT=8898
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

# 5 MiB of output: exceeds the 256 KiB tool-output cap, so the daemon must
# truncate the result and append a note the model (and this test) can see.
yes "nyaxus output cap test line" | head -c 5242880 > big.txt

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== create session =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
echo "$SID"

echo "== start run (mock provider calls the terminal tool) =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"use the terminal\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"
echo "$RUN1"

wait_approvals() {
    # $1 = minimum number of approval.requested events to wait for
    local want="$1" count=""
    for _ in $(seq 1 250); do
        count="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -o '"event_type":"approval.requested"' | wc -l | tr -d ' ')"
        if [ -n "$count" ] && [ "$count" -ge "$want" ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

# The daemon requires the pending call's approval_id on approve/deny so a
# stale decision cannot land on a different pending call. The id is in the
# newest approval.requested event's payload.
latest_approval_id() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o '"approval_id":"[^"]*"' | tail -1 \
        | sed 's/^"approval_id":"//; s/"$//'
}

echo "== wait for the first approval request (rm one.txt) =="
wait_approvals 1 || fail "no approval.requested event appeared"
echo "approval requested"

echo "== approve with session scope (tool-level: all terminal calls for this session) =="
AID="$(latest_approval_id)"
[ -n "$AID" ] || fail "no approval id in the event payload"
curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"scope\":\"session\",\"approval_id\":\"$AID\"}" > /dev/null || fail "approve command failed"
echo "approved (session scope)"

echo "== remaining terminal calls (rm one.txt, rm two.txt, cat big.txt) must run without re-prompting =="
echo "== wait for run completion (must not hang on a second approval) =="
DONE=""
for _ in $(seq 1 250); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (likely blocked on an approval)"

echo "== assert event ledger =="
SNAP="$(curl -sf "$BASE/sessions/$SID")"
APPROVALS="$(printf '%s' "$SNAP" | grep -o '"event_type":"approval.requested"' | wc -l | tr -d ' ')"
COMPLETED_TOOLS="$(printf '%s' "$SNAP" | grep -o '"event_type":"tool.completed"' | wc -l | tr -d ' ')"
RUNS_COMPLETED="$(printf '%s' "$SNAP" | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')"

# The first approval was for "rm one.txt" (the first terminal call).
printf '%s' "$SNAP" | grep -q '"event_type":"approval.requested"[^}]*rm one.txt' \
    || fail "the approval was not for the first call (rm one.txt)"

printf '%s' "$SNAP" | grep -q 'bytes of output omitted' \
    || fail "the big.txt output was not reduced at the head+tail target"

echo "approval.requested: $APPROVALS (want 1)"
echo "tool.completed: $COMPLETED_TOOLS (want 4)"
echo "run.completed: $RUNS_COMPLETED (want 1)"

[ "$APPROVALS" = "1" ] || fail "expected exactly 1 approval.requested, got $APPROVALS"
[ "$COMPLETED_TOOLS" = "4" ] || fail "expected 4 tool.completed, got $COMPLETED_TOOLS"
[ "$RUNS_COMPLETED" = "1" ] || fail "expected 1 run.completed, got $RUNS_COMPLETED"

echo
echo "APPROVAL_SCOPE_TEST_PASSED"
