#!/usr/bin/env bash
# trim_conflict_test.sh - trim is guarded while a run is in flight.
#
# A mock provider holds each stream open for a while, so a run can be
# observed mid-flight. The test:
#   - trim while the run is active -> 409 (stop it first)
#   - non-trim rewind (branch selection) while the run is active -> 200
#   - after the run is cancelled, the same trim succeeds and removes the tail
#
# Usage: harness/tests/trim_conflict_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8941
DAEMON_PORT=8942
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

MOCK_PORT="$MOCK_PORT" MODE=cancel INTERRUPT_DELAY_MS=12000 \
    "$NODE" "$ROOT/harness/tests/mock_interrupted_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

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

wait_event() {
    local sid="$1" type="$2"
    local found=""
    for _ in $(seq 1 100); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q "\"event_type\":\"$type\""; then
            found=1
            break
        fi
        sleep 0.2
    done
    [ "$found" = "1" ] || fail "event $type did not appear"
}

# The assistant message a run persisted, by id (empty if none).
assistant_id_for_run() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const m = (s.messages || []).find((x) => x.role === "assistant" && x.run_id === process.argv[1]);
            process.stdout.write(m ? String(m.id) : "");
        });' "$2"
}

echo "== health =="
wait_health || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== build a completed first turn =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"first\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"
wait_event "$SID" "run.completed"
A1="$(assistant_id_for_run "$SID" "$RUN1")"
[ -n "$A1" ] || fail "no assistant message id for the first turn"
echo "first turn done"

echo "== start a second turn (stays in flight) =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"second\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not start"
wait_event "$SID" "run.started"
echo "run 2 in flight"

echo "== trim while the run is active must 409 =="
CODE="$(curl -s -o trim409.json -w '%{http_code}' -X POST "$BASE/sessions/$SID/rewind" \
    -H 'Content-Type: application/json' -d "{\"message_id\":$A1,\"trim\":true}")"
[ "$CODE" = "409" ] || fail "trim while active returned $CODE, want 409"
grep -q "cannot trim while a run is active" trim409.json || fail "409 body wrong: $(cat trim409.json)"
echo "trim blocked with 409"

echo "== non-trim rewind (branch selection) is still allowed =="
CODE="$(curl -s -o rewind200.json -w '%{http_code}' -X POST "$BASE/sessions/$SID/rewind" \
    -H 'Content-Type: application/json' -d "{\"message_id\":$A1}")"
[ "$CODE" = "200" ] || fail "non-trim rewind while active returned $CODE, want 200"
echo "branch selection allowed"

echo "== cancel the run, then the same trim succeeds =="
curl -sf -X POST "$BASE/runs/$RUN2/commands" -H 'Content-Type: application/json' \
    -d '{"command":"cancel"}' > /dev/null || fail "cancel command failed"
wait_event "$SID" "run.cancelled"
CODE="$(curl -s -o trim200.json -w '%{http_code}' -X POST "$BASE/sessions/$SID/rewind" \
    -H 'Content-Type: application/json' -d "{\"message_id\":$A1,\"trim\":true}")"
[ "$CODE" = "200" ] || fail "trim after cancel returned $CODE, want 200"
grep -q '"trimmed":true' trim200.json || fail "trim result wrong: $(cat trim200.json)"
curl -sf "$BASE/sessions/$SID" | grep -q '"content":"second"' && fail "trimmed tail still present"
curl -sf "$BASE/sessions/$SID" | grep -q '"content":"first"' || fail "first turn missing after trim"
echo "trim after cancel removed the tail"

echo
echo "TRIM_CONFLICT_TEST_PASSED"
