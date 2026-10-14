#!/usr/bin/env bash
# End-to-end check for the terminal teardown receipt: when a
# command times out and the daemon kills its process tree, the log must
# answer "was the tree actually empty, or did we only kill the root?" -
# root_reaped / tree_is_empty / surviving pids.
#
# The mock provider answers with a terminal call that spawns three
# background sleeps and waits on them: the shell lives past the 60s tool
# timeout, so the kill path runs, and the receipt must report the whole
# tree gone (the job object / process group takes the grandchildren too).
# The leading "echo started" keeps the attempt off the silent-spawn retry
# (which would double the kill and the wait window).
#
# Usage: harness/tests/teardown_test.sh [path-to-nyaxus_daemon]
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

echo "== health =="
MOCK_PORT="$MOCK_PORT" \
TOOLS='[{"name":"terminal","arguments":{"command":"echo started; sleep 90 & sleep 90 & sleep 90; wait"}}]' \
"$NODE" "$ROOT/harness/tests/mock_tasks_loop.mjs" > mock.log 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 25); do
    kill -0 "$MOCK_PID" 2>/dev/null || break
    sleep 0.2
done
kill -0 "$MOCK_PID" 2>/dev/null || fail "mock process died (port $MOCK_PORT in use?) - see $WORKDIR/mock.log"

e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 75); do
    curl -sf "$BASE/health" > /dev/null && break
    sleep 0.2
done
kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon process died (port $DAEMON_PORT in use?) - see $WORKDIR/daemon.log"
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "ok"

echo "== run: command outlives the 60s tool timeout -> kill + receipt =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"run it\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"
# The command (background jobs + wait) is not readonly-classified, so it
# gates: approve it once, then the kill happens at the 60s tool timeout.
AID=""
for _ in $(seq 1 150); do
    AID="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -o '"approval_id":"[^"]*"' | tail -1 | sed 's/^"approval_id":"//; s/"$//')"
    [ -n "$AID" ] && break
    sleep 0.2
done
[ -n "$AID" ] || fail "no approval.requested appeared (the terminal call should gate)"
curl -sf -X POST "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$AID\"}" > /dev/null \
    || fail "approve command failed"
# The kill happens at the 60s tool timeout; the run completes right after.
# (No silent-spawn retry: the command produced output, so 60s + margin.)
for _ in $(seq 1 450); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"' \
    || fail "run did not complete (kill path?) - see $WORKDIR/daemon.log"
curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q 'timed out' \
    || fail "the tool result does not report the timeout"

echo "== receipt: tree actually empty =="
grep -q 'terminal teardown receipt pid=' "$WORKDIR/daemon.log" \
    || fail "no teardown receipt in daemon.log"
grep 'terminal teardown receipt' "$WORKDIR/daemon.log" \
    | grep -q 'root_reaped=true' \
    || fail "receipt says the root was not reaped"
grep 'terminal teardown receipt' "$WORKDIR/daemon.log" \
    | grep -q 'tree_is_empty=true' \
    || fail "receipt says the tree was not empty"
grep 'terminal teardown receipt' "$WORKDIR/daemon.log" \
    | grep -q 'surviving=none' \
    || fail "receipt reports surviving pids: $(grep 'terminal teardown receipt' "$WORKDIR/daemon.log")"

echo
echo "TEARDOWN_TEST_PASSED"
