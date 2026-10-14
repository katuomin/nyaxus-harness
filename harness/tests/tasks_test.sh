#!/usr/bin/env bash
# End-to-end check for the visible task-checklist tools.
#
# A mock OpenAI-compatible provider answers its first three completions
# with "tasks.create", "tasks.update", and a discard (tasks.create with an
# empty array), then plain text. Both tools are read-only, so neither may
# raise an approval gate. The checklist is ledgered (tasks.created
# replaces, tasks.updated toggles; an empty create discards) and rebuilt by
# the web from the event stream.
#
# Usage: harness/tests/tasks_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8971
DAEMON_PORT=8972
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PID=""
DAEMON_PID=""
cleanup() {
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
}
trap cleanup EXIT

TOOLS='[{"name":"tasks.create","arguments":{"tasks":"[\"step one\",\"step two\"]"}},{"name":"tasks.update","arguments":{"task":"1","status":"done"}},{"name":"tasks.create","arguments":{"tasks":"[]"}}]'

start_mock() {
    MOCK_PORT="$MOCK_PORT" TOOLS="$TOOLS" "$NODE" "$ROOT/harness/tests/mock_tasks_loop.mjs" > mock.log 2>&1 &
    MOCK_PID=$!
    for _ in $(seq 1 25); do
        kill -0 "$MOCK_PID" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$MOCK_PID" 2>/dev/null || fail "mock process died (port $MOCK_PORT in use?) - see $WORKDIR/mock.log"
}

start_daemon() {
    "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
    DAEMON_PID=$!
    for _ in $(seq 1 75); do
        curl -sf "$BASE/health" > /dev/null && break
        sleep 0.2
    done
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon process died (port $DAEMON_PORT in use?) - see $WORKDIR/daemon.log"
}

count_events() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o "\"event_type\":\"$1\"" | wc -l | tr -d ' '
}

session_ledger() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null
}

wait_run_done() {
    for _ in $(seq 1 350); do
        if [ "$(count_events run.completed)" -ge 1 ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
start_mock
start_daemon
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "ok"

echo "== run: tasks.create + tasks.update + discard, no gate =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"run it\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"
wait_run_done || fail "run did not complete"

CREATED="$(count_events tasks.created)"
echo "tasks.created: $CREATED (want 2)"
[ "$CREATED" = "2" ] || fail "expected exactly 2 tasks.created, got $CREATED"
session_ledger | grep -q '"event_type":"tasks.created"' \
    && session_ledger | grep -q 'step one' \
    || fail "tasks.created payload is missing the created tasks"

DISCARDED="$(session_ledger | grep -o '"tasks":"\[\]"' | wc -l | tr -d ' ')"
echo "discard payload: $DISCARDED (want 1)"
[ "$DISCARDED" = "1" ] || fail "expected 1 tasks.created with an empty list (discard)"

UPDATED="$(count_events tasks.updated)"
echo "tasks.updated: $UPDATED (want 1)"
[ "$UPDATED" = "1" ] || fail "expected exactly 1 tasks.updated, got $UPDATED"
session_ledger | grep -q '"event_type":"tasks.updated"' \
    && session_ledger | grep -q '"task":"1"' \
    && session_ledger | grep -q '"status":"done"' \
    || fail "tasks.updated payload is missing task/status"

TOOL_DONE="$(count_events tool.completed)"
echo "tool.completed: $TOOL_DONE (want 3)"
[ "$TOOL_DONE" = "3" ] || fail "expected 3 tool.completed (one per task tool), got $TOOL_DONE"

APPROVALS="$(count_events approval.requested)"
echo "approval.requested: $APPROVALS (want 0)"
[ "$APPROVALS" = "0" ] || fail "task tools are read-only and must not gate"

echo
echo "TASKS_TEST_PASSED"
