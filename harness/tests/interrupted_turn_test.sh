#!/usr/bin/env bash
# interrupted_turn_test.sh - partial responses survive interruption.
#
# Covers:
#   - cancelling a run mid-stream persists the partial assistant message
#     (durable, active, carrying the run's id), so it survives a reload and
#     the UI's [response interrupted] marker has a message to attach to
#   - a run that fails after a partial iteration (the tool loop's second
#     provider call 503s) persists the partial text with run.failed
#   - cancelling a run while the provider is silent (a long prompt pass, no
#     first token) ends the run within one 2s socket poll - not at the end
#     of the silence
#   - a run in flight when the daemon is killed is closed with run.failed at
#     the next startup, so event replay terminates (no phantom "working")
#
# Usage: harness/tests/interrupted_turn_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_A_PORT=8930
MOCK_B_PORT=8931
MOCK_C_PORT=8934
MOCK_D_PORT=8937
DAEMON_A_PORT=8932
DAEMON_B_PORT=8933
DAEMON_C_PORT=8935
DAEMON_D_PORT=8936
BASE_A="http://127.0.0.1:$DAEMON_A_PORT/api/v1"
BASE_B="http://127.0.0.1:$DAEMON_B_PORT/api/v1"
BASE_C="http://127.0.0.1:$DAEMON_C_PORT/api/v1"
BASE_D="http://127.0.0.1:$DAEMON_D_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemons run from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
# Each daemon stores nyaxus.db in its CWD, so they need separate directories.
mkdir -p da_a da_b da_c da_d

MOCK_PORT="$MOCK_A_PORT" MODE=cancel INTERRUPT_DELAY_MS=4000 \
    "$NODE" "$ROOT/harness/tests/mock_interrupted_provider.mjs" > mock_a.log 2>&1 &
APID=$!
MOCK_PORT="$MOCK_B_PORT" MODE=toolfail \
    "$NODE" "$ROOT/harness/tests/mock_interrupted_provider.mjs" > mock_b.log 2>&1 &
BPID=$!
MOCK_PORT="$MOCK_C_PORT" MODE=silent SILENT_MS=30000 \
    "$NODE" "$ROOT/harness/tests/mock_interrupted_provider.mjs" > mock_c.log 2>&1 &
CPID=$!
MOCK_PORT="$MOCK_D_PORT" MODE=silent SILENT_MS=30000 \
    "$NODE" "$ROOT/harness/tests/mock_interrupted_provider.mjs" > mock_d.log 2>&1 &
DPID=$!
( cd da_a && exec "$DAEMON" --port "$DAEMON_A_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_A_PORT/v1" --model mock-model ) > daemon_a.log 2>&1 &
DPID_A=$!
( cd da_b && exec "$DAEMON" --port "$DAEMON_B_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_B_PORT/v1" --model mock-model ) > daemon_b.log 2>&1 &
DPID_B=$!
( cd da_c && exec "$DAEMON" --port "$DAEMON_C_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_C_PORT/v1" --model mock-model ) > daemon_c.log 2>&1 &
DPID_C=$!
( cd da_d && exec "$DAEMON" --port "$DAEMON_D_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_D_PORT/v1" --model mock-model ) > daemon_d.log 2>&1 &
DPID_D=$!
trap 'kill "$DPID_A" "$DPID_B" "$DPID_C" "$DPID_D" "$APID" "$BPID" "$CPID" "$DPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

wait_health() {
    local base="$1"
    for _ in $(seq 1 50); do
        if curl -sf "$base/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
wait_health "$BASE_A" || fail "daemon A did not come up (see $WORKDIR/daemon_a.log)"
wait_health "$BASE_B" || fail "daemon B did not come up (see $WORKDIR/daemon_b.log)"
wait_health "$BASE_C" || fail "daemon C did not come up (see $WORKDIR/daemon_c.log)"
wait_health "$BASE_D" || fail "daemon D did not come up (see $WORKDIR/daemon_d.log)"
echo "ok"

wait_event() {
    local base="$1" sid="$2" type="$3"
    local found=""
    for _ in $(seq 1 100); do
        if curl -sf "$base/sessions/$sid" 2>/dev/null | grep -q "\"event_type\":\"$type\""; then
            found=1
            break
        fi
        sleep 0.2
    done
    [ "$found" = "1" ] || fail "event $type did not appear in $sid"
}

# The assistant message a run persisted, as JSON (empty if none).
assistant_for_run() {
    curl -sf "$1/sessions/$2" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const m = (s.messages || []).find((x) => x.role === "assistant" && x.run_id === process.argv[1]);
            process.stdout.write(m ? JSON.stringify(m) : "");
        });' "$3"
}

approval_id() {
    curl -sf "$1/sessions/$2" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            const req = [...evs].reverse().find((e) => e.event_type === "approval.requested");
            process.stdout.write(req ? String(req.payload.approval_id || "") : "");
        });'
}

echo "== cancel mid-stream: the partial response must persist =="
SID_A="$(curl -sf -X POST "$BASE_A/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_A" ] || fail "no session id (A)"
RUN_A="$(curl -sf -X POST "$BASE_A/sessions/$SID_A/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_A\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_A" ] || fail "run A did not start"
# Wait until the first chunk is delivered and visible in the durable ledger.
wait_event "$BASE_A" "$SID_A" "text.delta"
curl -sf "$BASE_A/sessions/$SID_A" | grep -q "partial-one" || fail "first chunk not visible before cancel"
curl -sf -X POST "$BASE_A/runs/$RUN_A/commands" -H 'Content-Type: application/json' \
    -d '{"command":"cancel"}' > /dev/null || fail "cancel command failed"
wait_event "$BASE_A" "$SID_A" "run.cancelled"
MSG_A="$(assistant_for_run "$BASE_A" "$SID_A" "$RUN_A")"
[ -n "$MSG_A" ] || fail "partial assistant message not persisted for the cancelled run"
printf '%s' "$MSG_A" | grep -q '"content":"partial-one "' || fail "partial content wrong: $MSG_A"
printf '%s' "$MSG_A" | grep -q '"is_active":true' || fail "partial message not active: $MSG_A"
printf '%s' "$MSG_A" | grep -q "partial-two" && fail "second chunk leaked into the partial: $MSG_A"
echo "cancelled partial persisted"

echo "== failure after a partial iteration: the partial must persist =="
SID_B="$(curl -sf -X POST "$BASE_B/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_B" ] || fail "no session id (B)"
RUN_B="$(curl -sf -X POST "$BASE_B/sessions/$SID_B/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_B\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_B" ] || fail "run B did not start"
wait_event "$BASE_B" "$SID_B" "approval.requested"
APR="$(approval_id "$BASE_B" "$SID_B")"
[ -n "$APR" ] || fail "no approval id in the ledger"
curl -sf -X POST "$BASE_B/runs/$RUN_B/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APR\",\"scope\":\"once\"}" > /dev/null \
    || fail "approve command failed"
wait_event "$BASE_B" "$SID_B" "run.failed"
MSG_B="$(assistant_for_run "$BASE_B" "$SID_B" "$RUN_B")"
[ -n "$MSG_B" ] || fail "partial assistant message not persisted for the failed run"
printf '%s' "$MSG_B" | grep -q '"content":"partial-text "' || fail "partial content wrong: $MSG_B"
printf '%s' "$MSG_B" | grep -q '"is_active":true' || fail "partial message not active: $MSG_B"
echo "failed partial persisted"

echo "== cancel while the provider is silent: the run must end promptly =="
# The mock answers 30s after the request; the 20s wait_event budget below is
# the regression bound: a cancel noticed only when the silence ends (the old
# single blocked read) lands at ~31s and fails the test.
SID_C="$(curl -sf -X POST "$BASE_C/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_C" ] || fail "no session id (C)"
RUN_C="$(curl -sf -X POST "$BASE_C/sessions/$SID_C/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_C\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_C" ] || fail "run C did not start"
sleep 1
curl -sf -X POST "$BASE_C/runs/$RUN_C/commands" -H 'Content-Type: application/json' \
    -d '{"command":"cancel"}' > /dev/null || fail "cancel command failed (C)"
wait_event "$BASE_C" "$SID_C" "run.cancelled"
echo "silent cancel landed"

echo "== daemon killed mid-run: the next startup must close the orphaned run =="
SID_D="$(curl -sf -X POST "$BASE_D/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_D" ] || fail "no session id (D)"
RUN_D="$(curl -sf -X POST "$BASE_D/sessions/$SID_D/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_D\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_D" ] || fail "run D did not start"
# The mock goes silent for 30s: the run is in flight (run.started in the
# ledger) when the daemon is hard-killed below.
wait_event "$BASE_D" "$SID_D" "run.started"
kill -9 "$DPID_D"
sleep 0.5
( cd da_d && exec "$DAEMON" --port "$DAEMON_D_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_D_PORT/v1" --model mock-model ) > daemon_d2.log 2>&1 &
DPID_D=$!
wait_health "$BASE_D" || fail "daemon D did not come back up (see $WORKDIR/daemon_d2.log)"
# The startup repair appends run.failed to the ledger, so the session
# snapshot (event replay) ends in a terminal state.
wait_event "$BASE_D" "$SID_D" "run.failed"
curl -sf "$BASE_D/sessions/$SID_D" | grep -q "interrupted by a daemon restart" \
    || fail "run.failed lacks the daemon-restart reason"
echo "orphaned run closed at startup"

echo "== regenerate a run that only exists in the ledger: must recover =="
# The dead run persists as its user message row; the retry must recover the
# turn from those rows instead of 404ing "run not found".
RESULT="$(curl -sf -X POST "$BASE_D/runs/$RUN_D/commands" \
    -H 'Content-Type: application/json' -d '{"command":"regenerate"}')"
echo "$RESULT" | grep -q '"run_id":"run-' || fail "regenerate of a restart-closed run failed: $RESULT"
NEW_RUN="$(echo "$RESULT" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$NEW_RUN" ] && [ "$NEW_RUN" != "$RUN_D" ] || fail "regenerate did not mint a new run: $RESULT"
# The recovered turn hangs on the silent provider: cancel it.
curl -sf -X POST "$BASE_D/runs/$NEW_RUN/commands" \
    -H 'Content-Type: application/json' -d '{"command":"cancel"}' > /dev/null
sleep 2
curl -sf "$BASE_D/sessions/$SID_D" | grep -q "$NEW_RUN" \
    || fail "recovered run did not reach the session"
echo "ledger-only run recovered by regenerate"

echo
echo "INTERRUPTED_TURN_TEST_PASSED"
