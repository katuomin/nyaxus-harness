#!/usr/bin/env bash
# memory_confidence_test.sh - validation/rejection confidence lifecycle
# (design principle 5).
#
# Covers:
#   - a unit starts at the default confidence 0.5
#   - positive use signals raise confidence: used (+0.05) + acted-on (+0.05)
#     + a verifiable citation (+0.10) -> 0.70 after one turn
#   - recall alone (offered) does not move confidence
#   - rejection (invalidation) drops confidence to 0.1
#
# Usage: harness/tests/memory_confidence_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9009
DAEMON_PORT=9010
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
EMBED_MODEL="$ROOT/harness/models/nyx/qwen3-embedding-0.6b-int8.nyx"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$EMBED_MODEL" ] || { echo "embedding model not found: $EMBED_MODEL" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

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

unit_confidence() {
    curl -sf "$BASE/memory/units/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const u = JSON.parse(d).unit || {};
            process.stdout.write(String(u.confidence || 0));
        });'
}

# in_range <value> <low> <high> -> exit 0 when low <= value <= high
in_range() {
    "$NODE" -e '
        const v = Number(process.argv[1]);
        process.exit(v >= Number(process.argv[2]) && v <= Number(process.argv[3]) ? 0 : 1);' "$1" "$2" "$3"
}

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_memory_confidence_provider.mjs" > provider.log 2>&1 &
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

seed_approved() {
    local sid="$1" text="$2"
    local record_id code
    record_id="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
        -d "{\"text\":\"$text\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$sid\"}" \
        | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
    [ -n "$record_id" ] || fail "record was not created"
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$record_id/approve" -d '{}')"
    [ "$code" = "200" ] || fail "approve returned $code"
}

echo "== seed one approved unit =="
SID_A="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_A" ] || fail "no session id"
seed_approved "$SID_A" "The zephyr protocol listener binds port 4242 on the gateway."
DRAINED=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 1)" = "1" ]; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "approval retain job did not complete: $(curl -sf "$BASE/memory/status")"
UNIT="$(bank_unit_id "zephyr protocol")"
[ -n "$UNIT" ] || fail "no unit id for the zephyr fact"
echo "unit $UNIT"

echo "== baseline: default confidence 0.5 =="
BASE_CONF="$(unit_confidence "$UNIT")"
in_range "$BASE_CONF" 0.49 0.51 || fail "expected baseline confidence ~0.5, got '$BASE_CONF'"
echo "baseline confidence=$BASE_CONF"

echo "== a run uses + acts on + cites the unit; confidence rises =="
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

echo "== confidence rose to ~0.70 (0.5 + 0.05 used + 0.05 acted + 0.10 cited) =="
USED_CONF="$(unit_confidence "$UNIT")"
in_range "$USED_CONF" 0.69 0.71 || fail "expected confidence ~0.70 after signals, got '$USED_CONF'"
echo "confidence after signals=$USED_CONF"

echo "== invalidate the unit; confidence drops to 0.1 =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/units/$UNIT/invalidate" -H 'Content-Type: application/json' -d '{"reason":"no longer valid"}')"
[ "$CODE" = "200" ] || fail "invalidate returned $CODE"
REJ_CONF="$(unit_confidence "$UNIT")"
in_range "$REJ_CONF" 0.09 0.11 || fail "expected confidence ~0.1 after rejection, got '$REJ_CONF'"
echo "confidence after rejection=$REJ_CONF"

echo
echo "MEMORY_CONFIDENCE_TEST_PASSED"
