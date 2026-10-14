#!/usr/bin/env bash
# End-to-end check for the durable command plane (docs/command-plane.md):
# steer/cancel carry a client-generated command id + issue time + TTL, and
# the daemon is the sole outcome writer.
#
#   A. steer with command id X -> 200, the run ends at the boundary
#   B. steer with the SAME id X -> 200 "skipped" (idempotent re-delivery,
#      no double effect)
#   C. steer issued 121s ago with a 120s TTL -> 410 Gone (expired: its
#      state is long gone)
#
# The mock provider answers with a `sleep 5` terminal call so the run is
# Running (steer-able) while the commands land.
#
# Usage: harness/tests/commandplane_test.sh [path-to-nyaxus_daemon]
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
TOOLS='[{"name":"terminal","arguments":{"command":"sleep 5"}}]' \
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

echo "== run: sleep 5 keeps the run steer-able =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"run it\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"
# sleep is readonly-classified, so it auto-approves and the run goes
# Running under the sleep. Wait for the tool to start: the 5s steer
# window is open from here.
for _ in $(seq 1 150); do
    curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"tool.started"' && break
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"tool.started"' \
    || fail "the sleep tool did not start"
NOW_MS="$(date +%s%3N)"
OLD_MS=$((NOW_MS - 121000))

echo "== phase A: steer with command id X =="
RESP="$(curl -sf -X POST "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"steer\",\"command_id\":\"cmd-x\",\"issued_at_ms\":$NOW_MS,\"ttl_ms\":120000}")"
echo "steer X: $RESP"

echo "== phase B: same id X again -> skipped =="
RESP="$(curl -sf -X POST "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"steer\",\"command_id\":\"cmd-x\",\"issued_at_ms\":$NOW_MS,\"ttl_ms\":120000}")"
echo "steer X again: $RESP"
echo "$RESP" | grep -q '"status":"skip"' \
    || fail "re-delivered command id did not evaluate to skip: $RESP"

echo "== phase C: stale steer (121s old, 120s TTL) -> 410 Gone =="
CODE="$(curl -s -o /tmp/cp_resp.txt -w '%{http_code}' -X POST "$BASE/runs/$RUN/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"steer\",\"command_id\":\"cmd-stale\",\"issued_at_ms\":$OLD_MS,\"ttl_ms\":120000}")"
BODY="$(cat /tmp/cp_resp.txt)"
echo "stale steer: $CODE $BODY"
[ "$CODE" = "410" ] || fail "expired steer should be 410 Gone, got $CODE"
echo "$BODY" | grep -q '"status":"expired"' \
    || fail "expired steer body is wrong: $BODY"

echo
echo "COMMANDPLANE_TEST_PASSED"
