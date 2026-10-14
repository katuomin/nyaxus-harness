#!/usr/bin/env bash
# memory_used_test.sh - used-memory feedback signal (design principle 5).
#
# Covers:
#   - the model may mark offered memories as used via memory.use
#   - the daemon validates against the offered set: a unit that was not
#     offered this turn (out of scope for the session) cannot be marked used
#   - the unit detail surfaces the used count and the last-used time
#
# Usage: harness/tests/memory_used_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9003
DAEMON_PORT=9004
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
EMBED_MODEL="$ROOT/harness/models/nyx/qwen3-embedding-0.6b-int8.nyx"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$EMBED_MODEL" ] || { echo "embedding model not found: $EMBED_MODEL" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# drained_ok <min-completed> -> 1 when at least N jobs completed and none
# pending/sending (background sweep jobs complete alongside retains, so the
# count is a lower bound, not exact).
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

# bank_unit_id <text-substring> -> the unit id whose text contains the marker
bank_unit_id() {
    curl -sf "$BASE/memory/bank?limit=50" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const bank = JSON.parse(d);
            const unit = (bank.units || []).find((u) => (u.text || "").includes(process.argv[1]));
            process.stdout.write(unit ? String(unit.id).replace(/^mem:/, "") : "");
        });' "$1"
}

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_memory_use_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
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
for _ in $(seq 1 60); do
    STATUS="$(curl -sf "$BASE/memory/status")"
    if printf '%s' "$STATUS" | grep -q '"ready":true'; then
        READY=1
        break
    fi
    sleep 0.5
done
[ "$READY" = "1" ] || fail "custom provider never became ready: $STATUS"
echo "ready"

seed_approved() {  # seed_approved <session-id> <text>
    local sid="$1" text="$2"
    local record_id code
    record_id="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
        -d "{\"text\":\"$text\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$sid\"}" \
        | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
    [ -n "$record_id" ] || fail "record was not created"
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$record_id/approve" -d '{}')"
    [ "$code" = "200" ] || fail "approve returned $code"
}

echo "== seed one approved unit in each of two sessions =="
SID_A="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_A" ] || fail "no session id A"
SID_B="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_B" ] || fail "no session id B"
seed_approved "$SID_A" "The zephyr protocol listener binds port 4242 on the gateway."
seed_approved "$SID_B" "The quasar relay binds port 5555 on the edge."
DRAINED=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 2)" = "1" ]; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "approval retain jobs did not complete: $(curl -sf "$BASE/memory/status")"
UNIT_A="$(bank_unit_id "zephyr protocol")"
UNIT_B="$(bank_unit_id "quasar relay")"
[ -n "$UNIT_A" ] || fail "no unit id for the zephyr fact"
[ -n "$UNIT_B" ] || fail "no unit id for the quasar fact"
# The mock marks "mem:1" (expected offered) and "mem:2" (expected not
# offered); a fresh database assigns ids in insertion order.
[ "$UNIT_A" = "1" ] && [ "$UNIT_B" = "2" ] || fail "expected units 1 and 2, got '$UNIT_A' and '$UNIT_B'"
echo "units $UNIT_A (session A) and $UNIT_B (session B)"

echo "== baseline: no used feedback yet =="
for U in 1 2; do
    USED="$(curl -sf "$BASE/memory/units/$U" | sed -n 's/.*"used":\([0-9]*\).*/\1/p')"
    [ "$USED" = "0" ] || fail "expected 0 used before any turn (unit $U), got '$USED'"
done
echo "baseline ok"

echo "== a run offers the in-scope unit; the model marks both ids used =="
RUN="$(curl -sf -X POST "$BASE/sessions/$SID_A/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_A\",\"message\":\"what port does the zephyr protocol use?\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_A" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete"
echo "run completed"

echo "== the offered unit is marked used; the unoffered one is not =="
DETAIL_A="$(curl -sf "$BASE/memory/units/1")"
OFFERED_A="$(printf '%s' "$DETAIL_A" | sed -n 's/.*"offered":\([0-9]*\).*/\1/p')"
USED_A="$(printf '%s' "$DETAIL_A" | sed -n 's/.*"used":\([0-9]*\).*/\1/p')"
[ "$OFFERED_A" = "1" ] || fail "expected unit 1 offered once, got '${OFFERED_A:-0}': $DETAIL_A"
[ "$USED_A" = "1" ] || fail "expected unit 1 used once, got '${USED_A:-0}': $DETAIL_A"
printf '%s' "$DETAIL_A" | grep -q '"last_used_at":"[^"]\{4,\}"' || fail "last_used_at missing: $DETAIL_A"
echo "unit 1 offered=$OFFERED_A used=$USED_A"

DETAIL_B="$(curl -sf "$BASE/memory/units/2")"
OFFERED_B="$(printf '%s' "$DETAIL_B" | sed -n 's/.*"offered":\([0-9]*\).*/\1/p')"
USED_B="$(printf '%s' "$DETAIL_B" | sed -n 's/.*"used":\([0-9]*\).*/\1/p')"
[ "$OFFERED_B" = "0" ] || fail "expected unit 2 never offered (out of scope), got '${OFFERED_B:-0}': $DETAIL_B"
[ "$USED_B" = "0" ] || fail "expected unit 2 never used (not offered), got '${USED_B:-0}': $DETAIL_B"
echo "unit 2 offered=$OFFERED_B used=$USED_B"

echo
echo "MEMORY_USED_TEST_PASSED"
