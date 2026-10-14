#!/usr/bin/env bash
# memory_operations_test.sh - custom memory unit curation e2e.
#
# Covers (memory operations/API, custom provider):
#   - unit detail: unit + version timeline + provenance (model id, reason)
#   - invalidate: unit leaves the active bank and recall, reason recorded
#   - reactivate: unit returns to the active bank and recall
#   - supersede: append-only new version, current text swaps, old text kept
#   - delete: hard remove, detail then 404s
#   - unknown unit ids 404, malformed ids 400
#
# Usage: harness/tests/memory_operations_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=8991
DAEMON_PORT=8992
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

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
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

echo "== seed one approved unit =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
    -d "{\"text\":\"The zephyr protocol listener binds port 4242 on the gateway.\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$SID\"}" \
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
BANK="$(curl -sf "$BASE/memory/bank?limit=50")"
UNIT_ID="$(printf '%s' "$BANK" | sed -n 's/.*"id":"mem:\([0-9]*\)".*/\1/p')"
[ -n "$UNIT_ID" ] || fail "no unit id in bank: $BANK"
echo "unit $UNIT_ID"

echo "== detail: unit, one version, provenance =="
DETAIL="$(curl -sf "$BASE/memory/units/$UNIT_ID")"
printf '%s' "$DETAIL" | grep -q "zephyr protocol listener" || fail "detail missing unit text: $DETAIL"
printf '%s' "$DETAIL" | grep -q '"state":"active"' || fail "detail not active: $DETAIL"
[ "$(printf '%s' "$DETAIL" | grep -o '"store_reason"' | wc -l)" = "1" ] || fail "expected exactly one version: $DETAIL"
printf '%s' "$DETAIL" | grep -q '"current":true' || fail "no current version: $DETAIL"
printf '%s' "$DETAIL" | grep -q "approved by operator" || fail "store reason missing: $DETAIL"
echo "detail ok"

echo "== invalidate: leaves active bank and recall, reason recorded =="
CODE="$(curl -s -o op.json -w '%{http_code}' -X POST "$BASE/memory/units/$UNIT_ID/invalidate" \
    -H 'Content-Type: application/json' -d '{"reason":"stale test fact"}')"
[ "$CODE" = "200" ] || fail "invalidate returned $CODE: $(cat op.json)"
BANK="$(curl -sf "$BASE/memory/bank?limit=50")"
printf '%s' "$BANK" | grep -q "zephyr" && fail "invalidated unit still in active bank: $BANK"
BANKALL="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
printf '%s' "$BANKALL" | grep -q '"state":"invalidated"' || fail "unit not invalidated: $BANKALL"
printf '%s' "$BANKALL" | grep -q "stale test fact" || fail "state reason missing: $BANKALL"
RECALL="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"what port does the zephyr protocol use?"}')"
# Match the unit's text, not the echoed query (which also says "zephyr").
printf '%s' "$RECALL" | grep -q "zephyr protocol listener" && fail "invalidated unit still recalled: $RECALL"
echo "invalidate ok"

echo "== reactivate: returns to active bank and recall =="
CODE="$(curl -s -o op.json -w '%{http_code}' -X POST "$BASE/memory/units/$UNIT_ID/reactivate" \
    -H 'Content-Type: application/json' -d '{"reason":"checked, still true"}')"
[ "$CODE" = "200" ] || fail "reactivate returned $CODE: $(cat op.json)"
BANK="$(curl -sf "$BASE/memory/bank?limit=50")"
printf '%s' "$BANK" | grep -q "zephyr protocol listener" || fail "reactivated unit missing from bank: $BANK"
printf '%s' "$BANK" | grep -q '"state":"active"' || fail "unit not active: $BANK"
RECALL="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"what port does the zephyr protocol use?"}')"
printf '%s' "$RECALL" | grep -q "zephyr protocol listener" || fail "reactivated unit not recalled: $RECALL"
echo "reactivate ok"

echo "== supersede: append-only new version, current text swaps =="
CODE="$(curl -s -o op.json -w '%{http_code}' -X POST "$BASE/memory/units/$UNIT_ID/supersede" \
    -H 'Content-Type: application/json' \
    -d '{"text":"The zephyr protocol listener binds port 9999 on the gateway.","reason":"port moved"}')"
[ "$CODE" = "200" ] || fail "supersede returned $CODE: $(cat op.json)"
DETAIL="$(curl -sf "$BASE/memory/units/$UNIT_ID")"
[ "$(printf '%s' "$DETAIL" | grep -o '"store_reason"' | wc -l)" = "2" ] || fail "expected two versions: $DETAIL"
[ "$(printf '%s' "$DETAIL" | grep -o '"current":true' | wc -l)" = "1" ] || fail "expected one current version: $DETAIL"
printf '%s' "$DETAIL" | grep -q "port 4242" || fail "old version lost from history: $DETAIL"
printf '%s' "$DETAIL" | grep -q "port 9999" || fail "new version missing: $DETAIL"
BANK="$(curl -sf "$BASE/memory/bank?limit=50")"
printf '%s' "$BANK" | grep -q "port 9999" || fail "bank does not show new text: $BANK"
printf '%s' "$BANK" | grep -q "port 4242" && fail "bank still shows old text: $BANK"
RECALL="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"what port does the zephyr protocol use?"}')"
printf '%s' "$RECALL" | grep -q "port 9999" || fail "recall does not return new text: $RECALL"
printf '%s' "$RECALL" | grep -q "port 4242" && fail "recall still returns old text: $RECALL"
echo "supersede ok"

echo "== delete: hard remove, then 404 =="
CODE="$(curl -s -o op.json -w '%{http_code}' -X POST "$BASE/memory/units/$UNIT_ID/delete" -d '{}')"
[ "$CODE" = "200" ] || fail "delete returned $CODE: $(cat op.json)"
BANKALL="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
printf '%s' "$BANKALL" | grep -q "zephyr" && fail "deleted unit still in bank: $BANKALL"
CODE="$(curl -s -o detail.json -w '%{http_code}' "$BASE/memory/units/$UNIT_ID")"
[ "$CODE" = "404" ] || fail "detail after delete expected 404, got $CODE: $(cat detail.json)"
echo "delete ok"

echo "== unknown and malformed ids =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/memory/units/999999")"
[ "$CODE" = "404" ] || fail "unknown detail expected 404, got $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/units/999999/invalidate" -d '{}')"
[ "$CODE" = "404" ] || fail "unknown invalidate expected 404, got $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/units/999999/supersede" -d '{"text":"x"}')"
[ "$CODE" = "404" ] || fail "unknown supersede expected 404, got $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/units/999999/delete" -d '{}')"
[ "$CODE" = "404" ] || fail "unknown delete expected 404, got $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/memory/units/not-a-number")"
[ "$CODE" = "400" ] || fail "malformed id expected 400, got $CODE"
echo "error paths ok"

echo
echo "MEMORY_OPERATIONS_TEST_PASSED"
