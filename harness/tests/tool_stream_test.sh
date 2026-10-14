#!/usr/bin/env bash
# tool_stream_test.sh - streamed tool-call aggregation and approval guards.
#
# Covers:
#   - fragmented SSE tool-call arguments are aggregated by index/id into a
#     single valid call (no bogus per-fragment dispatch, no invalid-JSON 500)
#   - a response carrying two tool calls round-trips both (approve, execute,
#     result for each)
#   - approving with a wrong approval id is rejected (409), gate stays pending
#   - a duplicate approval after the gate is decided is rejected (409)
#   - the ledger carries the per-step sequence in order (reasoning -> tools
#     -> reasoning -> tools -> reasoning -> text), with each step's deltas
#     consolidated into complete events
#   - the session's persisted updated_at moves forward after a run, so the
#     sidebar's recent-activity sort has a stable source
#
# Usage: harness/tests/tool_stream_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8940
DAEMON_PORT=8941
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
# The daemon stores nyaxus.db in its CWD.
mkdir -p da

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_fragment_tool_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
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

# Poll the session ledger until a raw pattern shows up (bounded).
wait_pattern() {
    local sid="$1" pattern="$2"
    for _ in $(seq 1 100); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q "$pattern"; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

# The approval id currently pending in the ledger (most recent request).
pending_approval() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            const req = [...evs].reverse().find((e) => e.event_type === "approval.requested");
            process.stdout.write(req ? String(req.payload.approval_id || "") : "");
        });'
}

# Approve the pending gate; prints the HTTP status of the command.
approve() {
    local run="$1" apr="$2"
    curl -s -o "$WORKDIR/approve.json" -w "%{http_code}" -X POST "$BASE/runs/$run/commands" \
        -H 'Content-Type: application/json' \
        -d "{\"command\":\"approve\",\"approval_id\":\"$apr\",\"scope\":\"once\"}"
}

session_updated_at() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            process.stdout.write(String(s.updated_at || ""));
        });'
}

echo "== health =="
wait_health || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
BEFORE="$(session_updated_at "$SID")"
[ -n "$BEFORE" ] || fail "session has no persisted updated_at"

RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"run the tools\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"

echo "== gate 1: fragmented tool call =="
wait_pattern "$SID" '"approval_id":"call_frag"' || fail "no approval.requested for the fragmented call"
APR1="$(pending_approval "$SID")"
[ "$APR1" = "call_frag" ] || fail "pending approval id wrong: $APR1"

# A wrong approval id must be rejected, and the gate must stay pending.
CODE="$(approve "$RUN" "call_wrong")"
[ "$CODE" = "409" ] || fail "wrong approval id accepted (got $CODE)"
wait_pattern "$SID" '"approval_id":"call_frag"' || fail "gate disappeared after a rejected approval"

CODE="$(approve "$RUN" "$APR1")"
[ "$CODE" = "200" ] || fail "approve failed (got $CODE)"
wait_pattern "$SID" '"tool_id":"call_frag"' || fail "fragmented call never completed"
curl -sf "$BASE/sessions/$SID" | grep -q "fragment ok" || fail "fragmented command output missing"
echo "fragmented call aggregated and executed"

echo "== gates 2+3: multi-call response =="
wait_pattern "$SID" '"approval_id":"call_a"' || fail "no approval.requested for call_a"
APRA="$(pending_approval "$SID")"
[ "$APRA" = "call_a" ] || fail "pending approval id wrong: $APRA"
CODE="$(approve "$RUN" "$APRA")"
[ "$CODE" = "200" ] || fail "approve call_a failed (got $CODE)"

wait_pattern "$SID" '"approval_id":"call_b"' || fail "no approval.requested for call_b"
APRB="$(pending_approval "$SID")"
[ "$APRB" = "call_b" ] || fail "pending approval id wrong: $APRB"
CODE="$(approve "$RUN" "$APRB")"
[ "$CODE" = "200" ] || fail "approve call_b failed (got $CODE)"

# A duplicate approval of an already-decided gate must be rejected.
CODE="$(approve "$RUN" "$APRB")"
[ "$CODE" = "409" ] || fail "duplicate approval accepted (got $CODE)"

wait_pattern "$SID" '"tool_id":"call_a"' || fail "call_a never completed"
wait_pattern "$SID" '"tool_id":"call_b"' || fail "call_b never completed"
curl -sf "$BASE/sessions/$SID" | grep -q "first-tool" || fail "call_a output missing"
curl -sf "$BASE/sessions/$SID" | grep -q "second-tool" || fail "call_b output missing"
echo "multi-call response round-tripped"

echo "== run completion =="
wait_pattern "$SID" '"event_type":"run.completed"' || fail "run did not complete"
curl -sf "$BASE/sessions/$SID" | grep -q "all tools done" || fail "final text missing"

echo "== per-step consolidated events =="
# The ledger must carry the per-step sequence in order: each provider call's
# reasoning, then its tool calls, then the final text - not one merged
# reasoning block after all the tools. Consolidated steps drop their deltas.
curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
      const evs = JSON.parse(d).events || [];
      const seq = evs
        .filter((e) => ["reasoning", "text", "tool.started", "run.completed"].includes(e.event_type))
        .map((e) => e.event_type);
      const expected = ["reasoning", "tool.started", "reasoning", "tool.started", "tool.started", "reasoning", "text", "run.completed"];
      const deltas = evs.filter((e) => e.event_type === "reasoning.delta" || e.event_type === "text.delta");
      process.exit(JSON.stringify(seq) === JSON.stringify(expected) && deltas.length === 0 ? 0 : 1);
    });' || fail "per-step event order wrong or deltas not consolidated"
echo "per-step events interleaved in order"

echo "== session timestamp moves after a run =="
AFTER="$(session_updated_at "$SID")"
[ -n "$AFTER" ] || fail "session lost its updated_at"
[ "$AFTER" != "$BEFORE" ] || fail "updated_at did not move after the run"
# updated_at is a nanosecond epoch (beyond Number.MAX_SAFE_INTEGER) - compare
# with BigInt.
"$NODE" -e "
    const before = BigInt(process.argv[1]), after = BigInt(process.argv[2]);
    process.exit(after > before ? 0 : 1);
" "$BEFORE" "$AFTER" || fail "updated_at went backwards"
echo "timestamp advanced"

echo
echo "TOOL_STREAM_TEST_PASSED"
