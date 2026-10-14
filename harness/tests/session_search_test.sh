#!/usr/bin/env bash
# End-to-end check for the session.search tool against the daemon.
#
# A mock OpenAI-compatible provider calls session.search twice, then answers
# with plain text:
#   request 1: mode=list  (every recorded session: id, title, message count)
#   request 2: mode=grep, query="needle" (the current session's user message
#             carries the phrase, so the grep must hit it)
#
# Both calls are read-only, so they must run without an approval gate.
# Asserts on the event ledger:
#   - two tool.completed (both calls executed)
#   - the list result carries the "sessions (" roster
#   - the grep result carries the matched line from the user message
#   - zero approval.requested (read-only tools skip the gate)
#   - one run.completed (the run did not hang)
#
# Usage: harness/tests/session_search_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8901
DAEMON_PORT=8902
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_session_search_provider.mjs" > mock.log 2>&1 &
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

echo "== start run (mock provider calls session.search twice) =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"find the needle text here\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"
echo "$RUN1"

echo "== wait for run completion (read-only tools must not prompt) =="
DONE=""
for _ in $(seq 1 75); do
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

printf '%s' "$SNAP" | grep -q 'sessions (' \
    || fail "the mode=list result did not carry the session roster"
printf '%s' "$SNAP" | grep -q "grep 'needle'" \
    || fail "the mode=grep result did not carry the query header"
printf '%s' "$SNAP" | grep -q 'find the needle text here' \
    || fail "the grep did not match the user message carrying the phrase"

echo "approval.requested: $APPROVALS (want 0)"
echo "tool.completed: $COMPLETED_TOOLS (want 2)"

[ "$APPROVALS" = "0" ] || fail "session.search must skip the approval gate, got $APPROVALS prompts"
[ "$COMPLETED_TOOLS" = "2" ] || fail "expected exactly 2 tool.completed, got $COMPLETED_TOOLS"

echo "SESSION_SEARCH_TEST_PASSED"
