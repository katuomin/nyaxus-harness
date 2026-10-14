#!/usr/bin/env bash
# End-to-end check for the runaway-loop detector:
# turn-local observation over EXECUTED calls. The pre-dispatch loop guard
# exempts read-only tools, so a recall/search poll that never changes is
# exactly the loop only the detector can see.
#
# Mock script: one run in which the model calls session.search three times
# with identical arguments (the current session's user message carries the
# query, so each grep returns the same result), then answers with text.
#
# Asserts:
#   - the 3rd identical call carries the [loop reminder] in its result
#   - one loop.guard ledger event: stage=reminder, kind=result-repeat
#   - zero approval.requested (read-only tools are ungated)
#   - run.completed (the reminder is a nudge, not a termination)
#
# Usage: harness/tests/runaway_loop_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9021
DAEMON_PORT=9022
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

SEARCH_CALL='{"name":"session.search","arguments":{"mode":"grep","query":"needle"}}'
CALLS="[${SEARCH_CALL},${SEARCH_CALL},${SEARCH_CALL},{\"text\":\"done\"}]"

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

CALLS="$CALLS" MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_turn_scope_provider.mjs" > mock.log 2>&1 &
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

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
echo "$SID"

echo "== run: three identical session.search greps =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"find the needle text here\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 150); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (the reminder must not terminate; see $WORKDIR/daemon.log)"
echo "run completed"

SNAP="$(curl -sf "$BASE/sessions/$SID")"

echo "== assert: reminder on the 3rd call, ledgered as result-repeat =="
printf '%s' "$SNAP" | grep -q '\[loop reminder\]' \
    || fail "no [loop reminder] in any tool result"
REMINDERS="$(printf '%s' "$SNAP" | grep -o '"stage":"reminder"' | wc -l | tr -d ' ')"
KINDS="$(printf '%s' "$SNAP" | grep -o '"kind":"result-repeat"' | wc -l | tr -d ' ')"
APPROVALS="$(printf '%s' "$SNAP" | grep -o '"event_type":"approval.requested"' | wc -l | tr -d ' ')"
TOOLS="$(printf '%s' "$SNAP" | grep -o '"event_type":"tool.completed"' | wc -l | tr -d ' ')"
echo "loop.guard reminders: $REMINDERS (want 1)"
echo "kind=result-repeat: $KINDS (want 1)"
echo "approval.requested: $APPROVALS (want 0)"
echo "tool.completed: $TOOLS (want 3)"
[ "$REMINDERS" = "1" ] || fail "expected exactly one reminder, got $REMINDERS"
[ "$KINDS" = "1" ] || fail "expected the result-repeat kind ledgered, got $KINDS"
[ "$APPROVALS" = "0" ] || fail "read-only tools must not prompt, got $APPROVALS"
[ "$TOOLS" = "3" ] || fail "all three searches must have run, got $TOOLS completed"
# The reminder rides the 3rd call's result (the 1st and 2nd are clean).
THIRD_RESULT="$(printf '%s' "$SNAP" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const tools = (s.events || []).filter((e) => e.event_type === "tool.completed");
        process.stdout.write(tools.length >= 3 ? String(tools[2].payload.result || "") : "");
    });')"
printf '%s' "$THIRD_RESULT" | grep -q '\[loop reminder\]' \
    || fail "the reminder must ride the 3rd call's result: $THIRD_RESULT"
FIRST_RESULT="$(printf '%s' "$SNAP" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const tools = (s.events || []).filter((e) => e.event_type === "tool.completed");
        process.stdout.write(tools.length >= 1 ? String(tools[0].payload.result || "") : "");
    });')"
printf '%s' "$FIRST_RESULT" | grep -q '\[loop reminder\]' \
    && fail "the 1st call must be clean (no reminder)"

echo "RUNAWAY_LOOP_TEST_PASSED"
