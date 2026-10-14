#!/usr/bin/env bash
# End-to-end check for resumable SSE replay against the daemon (fake provider).
#
# Verifies:
#   1. the session snapshot carries a global ledger id on every event
#   2. `GET /sessions/{id}/events?after=<cursor>` replays nothing already seen
#   3. a concurrent run started after connecting streams only new events,
#      each framed with an SSE `id:` line (so EventSource can resume via
#      Last-Event-ID)
#   4. a cursor-less connection replays the full ledger
#
# Usage: harness/tests/sse_resume_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
PORT=8899
BASE="http://127.0.0.1:$PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

"$DAEMON" --port "$PORT" > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" 2>/dev/null || true' EXIT
sleep 1

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== create session =="
SID_JSON="$(curl -sf -X POST "$BASE/sessions" -d '{}')"
SID="$(printf '%s' "$SID_JSON" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id in: $SID_JSON"
echo "$SID"

echo "== run 1 (fake provider) =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"
sleep 2

echo "== snapshot carries global event ids =="
SNAP="$(curl -sf "$BASE/sessions/$SID")"
MAXID="$(printf '%s' "$SNAP" | grep -oE '"id":[0-9]+' | grep -oE '[0-9]+' | sort -n | tail -1)"
[ -n "$MAXID" ] || fail "snapshot events have no global id"
printf '%s' "$SNAP" | grep -q "\"run_id\":\"$RUN1\"" || fail "run 1 missing from snapshot"
echo "max ledger id: $MAXID"

echo "== quiet stream: ?after=MAXID must not replay old events =="
timeout 3 curl -sN "$BASE/sessions/$SID/events?after=$MAXID" > sse_quiet.txt || true
QUIET_FRAMES="$(grep -c '^data: {' sse_quiet.txt || true)"
[ "$QUIET_FRAMES" = "0" ] || fail "expected 0 replayed frames, got $QUIET_FRAMES"
echo "ok (0 frames)"

echo "== resume stream: only run 2 events, each with an id: line =="
timeout 4 curl -sN "$BASE/sessions/$SID/events?after=$MAXID" > sse_resume.txt &
SSE_PID=$!
sleep 1
RUN2_JSON="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"second\"}")"
RUN2="$(printf '%s' "$RUN2_JSON" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not start"
wait "$SSE_PID" || true

grep -q "\"run_id\":\"$RUN2\"" sse_resume.txt || fail "run 2 events not received on resumed stream"
grep -q "\"run_id\":\"$RUN1\"" sse_resume.txt && fail "run 1 events were replayed on resumed stream"
DATA_FRAMES="$(grep -c '^data: {' sse_resume.txt || true)"
ID_FRAMES="$(grep -Ec '^id: [0-9]+' sse_resume.txt || true)"
[ "$DATA_FRAMES" -gt 0 ] || fail "no data frames on resumed stream"
[ "$ID_FRAMES" -ge "$DATA_FRAMES" ] || fail "not every data frame carries an id: line ($ID_FRAMES < $DATA_FRAMES)"
echo "ok ($DATA_FRAMES frames, all id-tagged)"

echo "== cursor-less stream replays the full ledger =="
timeout 3 curl -sN "$BASE/sessions/$SID/events" > sse_full.txt || true
grep -q "\"run_id\":\"$RUN1\"" sse_full.txt || fail "run 1 not replayed on cursor-less stream"
grep -q "\"run_id\":\"$RUN2\"" sse_full.txt || fail "run 2 not replayed on cursor-less stream"
echo "ok"

echo
echo "SSE_RESUME_TEST_PASSED"
