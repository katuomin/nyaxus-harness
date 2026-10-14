#!/usr/bin/env bash
# End-to-end check that a run's streaming deltas are collapsed into a single
# coalesced event once the run terminates. The per-chunk delta rows exist only
# to support mid-stream SSE resume; after the run ends the complete message is
# persisted, so the deltas are dropped and replaced by one `reasoning` and one
# `text` event each.
#
# Verifies:
#   1. after run.completed no reasoning.delta / text.delta rows remain
#   2. exactly one coalesced `reasoning` and one `text` event, each carrying
#      the full content
#   3. the assistant message is persisted with the full content
#
# Usage: harness/tests/delta_collapse_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PORT=8897
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
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
echo "$SID"

echo "== run (fake provider) =="
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"

# Wait for the run to terminate (run.completed lands in the durable ledger).
for _ in $(seq 1 50); do
    curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"run.completed"' && break
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"run.completed"' || fail "run did not complete"
echo "run completed"

# Count ledger events of a given type in the snapshot.
count_type() {
    curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            process.stdout.write(String(evs.filter((e) => e.event_type === process.argv[1]).length));
        });' "$1"
}

# Payload text of the first ledger event of a given type.
first_text() {
    curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            const e = evs.find((x) => x.event_type === process.argv[1]);
            process.stdout.write(e ? String(e.payload.text || "") : "");
        });' "$1"
}

echo "== deltas collapsed after completion =="
[ "$(count_type reasoning.delta)" = "0" ] || fail "reasoning.delta rows remain after completion"
[ "$(count_type text.delta)" = "0" ] || fail "text.delta rows remain after completion"
[ "$(count_type reasoning)" = "1" ] || fail "expected exactly one coalesced reasoning event, got $(count_type reasoning)"
[ "$(count_type text)" = "1" ] || fail "expected exactly one coalesced text event, got $(count_type text)"
echo "ok (0 deltas, 1 reasoning + 1 text)"

echo "== coalesced events carry the full content =="
TEXT="$(first_text text)"
grep -q "fake provider" <<< "$TEXT" || fail "coalesced text missing content: $TEXT"
REASONING="$(first_text reasoning)"
grep -q "fake provider" <<< "$REASONING" || fail "coalesced reasoning missing content: $REASONING"
echo "ok"

echo "== assistant message persisted =="
curl -sf "$BASE/sessions/$SID" | grep -q "fake provider" || fail "assistant message content missing"
echo "ok"

echo
echo "DELTA_COLLAPSE_TEST_PASSED"
