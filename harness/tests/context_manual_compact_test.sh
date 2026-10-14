#!/usr/bin/env bash
# context_manual_compact_test.sh - POST /sessions/{id}/context/compact:
# operator-triggered compaction ("Compress now"). In selected mode it folds
# the oldest turns into a new summary checkpoint (the budget guard's path,
# with the operator as the trigger); in summary mode it re-compresses the
# active tail (same as /context/recompress). Idle sessions only: a run in
# progress is 409.
#
# Usage: harness/tests/context_manual_compact_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8974
SLOW_PORT=8975
DAEMON_PORT=8976
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
# Provider window 131072 (mock /v1/models): the daemon's request budget is
# 128000 tokens (~256 KB at the 2 bytes/token fallback), so the seeded
# ~120 KB chain never trips the budget guard. The compaction tail budget is
# 40% of the window (~105 KB), so the split keeps the last ~5 messages and
# compacts the head.
FILLER="$(printf 'fillerword%.0s' $(seq 1 2000))"  # 20000 chars
TURNS=6

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
mkdir -p da
REQUEST_LOG="$WORKDIR/requests.jsonl"

MOCK_PORT="$MOCK_PORT" REQUEST_LOG="$REQUEST_LOG" \
    MAX_REQUEST_BYTES=2000000 \
    "$NODE" "$ROOT/harness/tests/mock_budget_provider.mjs" > mock.log 2>&1 &
MPID=$!
MOCK_PORT="$SLOW_PORT" MOCK_DELAY_MS=3000 \
    "$NODE" "$ROOT/harness/tests/mock_slow_provider.mjs" > slow.log 2>&1 &
SPID=$!
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model \
    --auxiliary "http://127.0.0.1:$MOCK_PORT/aux/v1" --auxiliary-model mock-aux \
    --auxiliary-no-thinking \
    --tool-timeout-ms 120000 ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" "$SPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; echo "--- daemon.log ---" >&2; tail -40 "$WORKDIR/daemon.log" >&2 2>/dev/null || true; echo "--- daemon2.log ---" >&2; tail -20 "$WORKDIR/daemon2.log" >&2 2>/dev/null || true; exit 1; }

wait_health() {
    for _ in $(seq 1 50); do
        if curl -sf "$BASE/health" > /dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
wait_health || fail "daemon did not come up"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

completed_runs() {
    curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            process.stdout.write(String(evs.filter((e) => e.event_type === "run.completed").length));
        });'
}

echo "== $TURNS seeded turns =="
for i in $(seq 1 $TURNS); do
    MARKER="$(printf 'MANUAL MARKER %04d' "$i")"
    RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$MARKER $FILLER\"}" | \
        sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$RUN" ] || fail "run $i did not start"
    for _ in $(seq 1 150); do
        N="$(completed_runs)"
        [ "${N:-0}" -ge "$i" ] && break
        sleep 0.2
    done
    N="$(completed_runs)"
    [ "${N:-0}" -ge "$i" ] || fail "run $i did not complete (completed=$N)"
done
echo "ok"

echo "== selected mode: manual compact =="
RESP="$(curl -sf -X POST "$BASE/sessions/$SID/context/compact")"
[ -n "$RESP" ] || fail "manual compact returned no body"
echo "$RESP" | grep -q '"status":"compacted"' || \
    fail "manual compact did not compact: $RESP"

curl -sf "$BASE/sessions/$SID" > session.json
grep -q '"context_mode":"summary"' session.json || \
    fail "session did not end in summary mode"
grep -q '"context_status":"ready"' session.json || \
    fail "context is not ready after manual compact"
grep -q 'context.compaction_started' session.json || \
    fail "no context.compaction_started event"
grep -q 'context.compacted' session.json || \
    fail "no context.compacted event"

# The auxiliary model summarized the head.
AUX_CALLS="$("$NODE" -e '
const fs = require("fs");
const lines = fs.readFileSync(process.argv[1], "utf8").trim().split(/\r?\n/).filter(Boolean);
process.stdout.write(String(lines.filter((line) => JSON.parse(line).endpoint === "aux").length));
' "$REQUEST_LOG")"
[ "${AUX_CALLS:-0}" -ge 1 ] || fail "auxiliary model was never called"
echo "ok"

echo "== summary mode: manual compact re-compresses the tail =="
# The first compaction's checkpoint covers only the head; the active tail
# (11 messages) is above the re-compression threshold, so the second manual
# compact folds the whole tail into the checkpoint.
RESP2="$(curl -sf -X POST "$BASE/sessions/$SID/context/compact")"
[ -n "$RESP2" ] || fail "second manual compact returned no body"
echo "$RESP2" | grep -q '"status":"recompressing"' || \
    fail "expected a re-compression, got: $RESP2"
for _ in $(seq 1 150); do
    curl -sf "$BASE/sessions/$SID" | grep -q '"context_status":"ready"' && break
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q 'context.recompressed' || \
    fail "no context.recompressed event after the tail re-compression"
echo "ok"

echo "== summary mode: empty tail reuses the checkpoint =="
# The re-compression covers the whole conversation, so a third manual
# compact finds no delta and reuses the checkpoint.
RESP3="$(curl -sf -X POST "$BASE/sessions/$SID/context/compact")"
[ -n "$RESP3" ] || fail "third manual compact returned no body"
echo "$RESP3" | grep -q '"status":"reused"' || \
    fail "expected the checkpoint to be reused, got: $RESP3"
echo "ok"

echo "== busy: 409 while a run is in progress =="
# The provider endpoint is fixed at startup, so restart the daemon against
# the slow mock (3 s delay keeps the run in flight across the compact call).
kill "$DPID" 2>/dev/null
wait "$DPID" 2>/dev/null
sleep 0.5
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$SLOW_PORT/v1" --model mock-model \
    --auxiliary "http://127.0.0.1:$MOCK_PORT/aux/v1" --auxiliary-model mock-aux \
    --auxiliary-no-thinking \
    --tool-timeout-ms 120000 ) > daemon2.log 2>&1 &
DPID=$!
wait_health || fail "daemon did not come back up"
SID2="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no session id (busy scenario)"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID2/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID2\",\"message\":\"slow turn\"}" | \
    sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "slow run did not start"
sleep 0.5
CODE="$(curl -s -o compact_busy.json -w "%{http_code}" -X POST \
    "$BASE/sessions/$SID2/context/compact")"
[ "$CODE" = "409" ] || fail "compact during a run expected 409, got $CODE: $(cat compact_busy.json)"
curl -sf -X POST "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
    -d '{"command":"cancel"}' > /dev/null || fail "cancelling the slow run failed"
echo "ok"

echo
echo "CONTEXT_MANUAL_COMPACT_TEST_PASSED"
