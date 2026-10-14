#!/usr/bin/env bash
# memory_recall_floor_test.sh - recall floor + honest no-data (Hillock).
#
# Covers:
#   - with memory.recall_floor off (default), a run recalls the seeded unit
#     and injects the past-evidence block (baseline)
#   - raising the floor above the unit's rerank logit live: the unit is
#     suppressed, /memory/recall reports suppressed_below_floor, and the
#     run injects the explicit "no stored memory matched" line instead of
#     priming the model with a near-miss
#   - lowering the floor back to 0 restores the recall (round trip)
#
# Usage: harness/tests/memory_recall_floor_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9011
DAEMON_PORT=9012
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
EMBED_MODEL="$ROOT/harness/models/nyx/qwen3-embedding-0.6b-int8.nyx"
RERANK_MODEL="$ROOT/harness/models/nyx/qwen3-reranker-0.6b-int8.nyx"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$EMBED_MODEL" ] || { echo "embedding model not found: $EMBED_MODEL" >&2; exit 1; }
[ -f "$RERANK_MODEL" ] || { echo "rerank model not found: $RERANK_MODEL" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# last_assistant <session-json> -> the newest assistant message content
last_assistant() {
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const msgs = (s.messages || []).filter((m) => m.role === "assistant");
            process.stdout.write(msgs.length ? (msgs[msgs.length - 1].content || "") : "");
        });'
}

# wait_for_marker <marker> -> echoes the newest assistant message once it
# contains <marker>; returns non-zero on timeout.
wait_for_marker() {
    local marker="$1"
    local _
    for _ in $(seq 1 75); do
        local session answer
        session="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null)" || continue
        answer="$(last_assistant "$session")"
        if printf '%s' "$answer" | grep -qF "$marker"; then
            printf '%s' "$answer"
            return 0
        fi
        sleep 0.2
    done
    return 1
}

# drained_ok <min-completed> -> 1 when at least N jobs completed and none
# pending/sending.
drained_ok() {
    curl -sf "$BASE/memory/status" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const jobs = (JSON.parse(d).jobs) || {};
            const waiting = Number(jobs.pending || 0) + Number(jobs.sending || 0);
            process.stdout.write(Number(jobs.completed || 0) >= Number(process.argv[1]) && waiting === 0 ? "1" : "0");
        });' "$1"
}

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_RERANK_MODEL="$RERANK_MODEL" \
NYAXUS_MEMORY_RECALL_ON_TURN=1 \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== custom provider becomes ready (lazy model load) =="
READY=""
for _ in $(seq 1 120); do
    STATUS="$(curl -sf "$BASE/memory/status")"
    if printf '%s' "$STATUS" | grep -q '"ready":true'; then
        READY=1
        break
    fi
    sleep 0.5
done
[ "$READY" = "1" ] || fail "custom provider never became ready: $STATUS"
echo "ready"

echo "== seed one approved unit (floor off by default) =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
    -d "{\"text\":\"The lighthouse keeper password is seagull-42.\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
[ -n "$RECORD_ID" ] || fail "record was not created"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$RECORD_ID/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE"
DRAINED=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 1)" = "1" ]; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "approval retain job did not complete: $(curl -sf "$BASE/memory/status")"
echo "unit stored"

echo "== baseline: the run recalls the unit and injects past evidence =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what is the lighthouse keeper password?\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"
ANSWER1="$(wait_for_marker 'seagull-42')" || fail "run 1 did not inject the recalled fact"
printf '%s' "$ANSWER1" | grep -q "Memory: past evidence" || fail "past-evidence marker missing: $ANSWER1"
echo "baseline injection ok"

echo "== raise the floor above the unit's rerank logit (live apply) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"memory.recall_floor":4.0}')"
[ "$CODE" = "200" ] || fail "could not set memory.recall_floor: $CODE"

echo "== /memory/recall reports the suppression =="
RECALL="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d "{\"query\":\"what is the lighthouse keeper password?\",\"session\":\"$SID\"}")"
printf '%s' "$RECALL" | grep -q '"source":"custom"' || fail "expected custom recall: $RECALL"
printf '%s' "$RECALL" | grep -q '"suppressed_below_floor":[1-9]' || fail "suppression count missing: $RECALL"
printf '%s' "$RECALL" | grep -q '"results":\[\]' || fail "expected empty results below the floor: $RECALL"
echo "suppression ok"

echo "== the run injects the honest no-data line instead of a near-miss =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what is the lighthouse keeper password?\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not start"
ANSWER2="$(wait_for_marker 'no stored memory matched this turn')" || fail "run 2 did not inject the no-data line"
printf '%s' "$ANSWER2" | grep -q 'seagull-42' && fail "suppressed fact leaked into the injection: $ANSWER2"
echo "no-data injection ok"

echo "== lower the floor back to 0: the recall round-trips =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"memory.recall_floor":0}')"
[ "$CODE" = "200" ] || fail "could not reset memory.recall_floor: $CODE"
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what is the lighthouse keeper password?\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "run 3 did not start"
ANSWER3="$(wait_for_marker 'seagull-42')" || fail "run 3 did not recall the fact after the floor reset"
printf '%s' "$ANSWER3" | grep -q "Memory: past evidence" || fail "past-evidence marker missing: $ANSWER3"
echo "round trip ok"

echo
echo "MEMORY_RECALL_FLOOR_TEST_PASSED"
