#!/usr/bin/env bash
# connection_recovery_test.sh - browser connection phase transitions (step 5).
#
# The browser moves through: loading session snapshot -> opening event stream
# -> connected -> disconnected (retry) -> connected. This test covers the
# daemon-side contract for the disconnected -> connected transition across a
# real daemon restart:
#
#   1. the snapshot still carries the full ledger after a restart
#   2. a stream resumed with `?after=<cursor>` replays exactly the events
#      emitted while the daemon was down, each with an SSE `id:` line
#   3. a stream resumed with the `Last-Event-ID` header (the EventSource
#      path) replays the same gap
#   4. resuming after an already-seen id replays nothing (no duplicates)
#   5. a run started after the reconnect streams only its own events
#
# Usage: harness/tests/connection_recovery_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PORT=8961
BASE="http://127.0.0.1:$PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

start_daemon() {
    "$DAEMON" --port "$PORT" > daemon.log 2>&1 &
    for _ in $(seq 1 50); do
        curl -sf "$BASE/health" > /dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

stop_daemon() { taskkill //F //IM nyaxus_daemon.exe > /dev/null 2>&1 || true; }

max_ledger_id() {
    curl -sf "$BASE/sessions/$1" | grep -oE '"id":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1
}

wait_run() { # wait_run <sid> <run-id>: until that run reaches a terminal event
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
            let d = "";
            process.stdin.on("data", (c) => { d += c; });
            process.stdin.on("end", () => {
                const s = JSON.parse(d);
                const rid = process.argv[1];
                const done = (s.events || []).some((e) =>
                    e.run_id === rid &&
                    (e.event_type === "run.completed" ||
                     e.event_type === "run.failed" ||
                     e.event_type === "run.cancelled"));
                process.exit(done ? 0 : 1);
            });' "$2" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

send() { # send <sid> <message> -> the new run id
    curl -sf -X POST "$BASE/sessions/$1/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$1\",\"message\":\"$2\"}" \
        | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p'
}

stop_daemon
start_daemon || fail "daemon did not come up (see $WORKDIR/daemon.log)"
trap 'stop_daemon' EXIT

echo "== run 1 completes on the first daemon instance =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(send "$SID" "before the drop")"
[ -n "$RUN1" ] || fail "run 1 rejected"
wait_run "$SID" "$RUN1" || fail "run 1 did not complete"
MAXID1="$(max_ledger_id "$SID")"
[ -n "$MAXID1" ] || fail "no ledger id after run 1"
echo "ledger at $MAXID1"

echo "== daemon drops (process killed) =="
stop_daemon
sleep 0.5

echo "== daemon comes back with the same store =="
start_daemon || fail "daemon did not come up after restart (see $WORKDIR/daemon.log)"
curl -sf "$BASE/sessions/$SID" | grep -q 'before the drop' || \
    fail "session content missing after restart"
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"run.completed"' || \
    fail "run 1 ledger missing after restart"
echo "snapshot intact"

echo "== run 2 completes while the browser is disconnected =="
RUN2="$(send "$SID" "during the gap")"
[ -n "$RUN2" ] || fail "run 2 rejected"
wait_run "$SID" "$RUN2" || fail "run 2 did not complete"
MAXID2="$(max_ledger_id "$SID")"
[ "$MAXID2" -gt "$MAXID1" ] || fail "ledger did not advance across the restart"
echo "ledger at $MAXID2"

echo "== resume with ?after= replays exactly the gap =="
timeout 3 curl -sN "$BASE/sessions/$SID/events?after=$MAXID1" > gap_query.txt || true
grep -q "\"run_id\":\"$RUN2\"" gap_query.txt || fail "run 2 events missing (query resume)"
grep -q "\"run_id\":\"$RUN1\"" gap_query.txt && fail "run 1 events were replayed (query resume)"
DATA_FRAMES="$(grep -c '^data: {' gap_query.txt || true)"
ID_FRAMES="$(grep -Ec '^id: [0-9]+' gap_query.txt || true)"
[ "$DATA_FRAMES" -gt 0 ] || fail "no data frames on the gap replay"
[ "$ID_FRAMES" -ge "$DATA_FRAMES" ] || fail "not every gap frame carries an id: line"
echo "ok ($DATA_FRAMES gap frames, all id-tagged)"

echo "== resume with Last-Event-ID replays the same gap =="
timeout 3 curl -sN -H "Last-Event-ID: $MAXID1" "$BASE/sessions/$SID/events" > gap_header.txt || true
grep -q "\"run_id\":\"$RUN2\"" gap_header.txt || fail "run 2 events missing (header resume)"
grep -q "\"run_id\":\"$RUN1\"" gap_header.txt && fail "run 1 events were replayed (header resume)"
echo "ok"

echo "== resuming at the current tip replays nothing =="
timeout 3 curl -sN "$BASE/sessions/$SID/events?after=$MAXID2" > tip.txt || true
QUIET_FRAMES="$(grep -c '^data: {' tip.txt || true)"
[ "$QUIET_FRAMES" = "0" ] || fail "expected 0 frames at the tip, got $QUIET_FRAMES"
echo "ok (0 frames)"

echo "== a run after the reconnect streams only its own events =="
timeout 4 curl -sN "$BASE/sessions/$SID/events?after=$MAXID2" > post.txt &
SSE_PID=$!
sleep 1
RUN3="$(send "$SID" "after the reconnect")"
[ -n "$RUN3" ] || fail "run 3 rejected"
wait "$SSE_PID" || true
wait_run "$SID" "$RUN3" || fail "run 3 did not complete"
grep -q "\"run_id\":\"$RUN3\"" post.txt || fail "run 3 events missing on the resumed stream"
grep -q "\"run_id\":\"$RUN2\"" post.txt && fail "run 2 events were replayed on the resumed stream"
echo "ok"

echo
echo "CONNECTION_RECOVERY_TEST_PASSED"
