#!/usr/bin/env bash
# memory_record_approval_test.sh - approved-memory queue promotion e2e.
#
# Covers (approval queue authority migration, custom provider):
#   - approving a record enqueues a durable retain job and the worker
#     materializes it into the custom memory bank (direct-text path, no
#     auxiliary model)
#   - the stored unit carries the operator's store_reason and the record's
#     model id, and is recallable whole-bank
#   - re-approving is idempotent (no second job, no duplicate unit)
#
# Usage: harness/tests/memory_record_approval_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=8981
DAEMON_PORT=8982
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

echo "== create a session-scoped candidate record =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
    -d "{\"text\":\"The zephyr protocol listener binds port 4242 on the gateway.\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
[ -n "$RECORD_ID" ] || fail "record was not created"
echo "record $RECORD_ID"

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

echo "== approve the record =="
CODE="$(curl -s -o approve.json -w '%{http_code}' -X POST "$BASE/memory/records/$RECORD_ID/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE: $(cat approve.json)"
grep -q '"status":"approved"' approve.json || fail "record not approved: $(cat approve.json)"
echo "approved"

echo "== wait for the worker to materialize the unit =="
DRAINED=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 1)" = "1" ]; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "approval retain job did not complete: $(curl -sf "$BASE/memory/status")"
echo "job completed"

echo "== the unit is in the bank with the operator's provenance =="
BANK="$(curl -sf "$BASE/memory/bank?limit=50")"
printf '%s' "$BANK" | grep -q "zephyr protocol listener" || fail "approved unit missing from bank: $BANK"
printf '%s' "$BANK" | grep -q "approved:record:$RECORD_ID" || fail "model id missing: $BANK"
printf '%s' "$BANK" | grep -q "approved by operator" || fail "store reason missing: $BANK"
TOTAL="$(printf '%s' "$BANK" | sed -n 's/.*"total":\([0-9]*\).*/\1/p')"
[ "$TOTAL" = "1" ] || fail "expected exactly 1 unit in the bank, got $TOTAL"
echo "bank ok"

echo "== the unit is recallable whole-bank =="
RECALL="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"what port does the zephyr protocol use?"}')"
printf '%s' "$RECALL" | grep -q '"source":"custom"' || fail "recall did not use the custom provider: $RECALL"
printf '%s' "$RECALL" | grep -q "zephyr protocol listener" || fail "approved unit not recalled: $RECALL"
echo "recall ok"

echo "== re-approval is idempotent (no duplicate job or unit) =="
# The completed count must not grow: a re-approve must not enqueue a second
# retain job (sweep jobs are excluded by comparing before/after, not exact).
job_count() {
    curl -sf "$BASE/memory/status" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            process.stdout.write(String((JSON.parse(d).jobs || {}).completed || 0));
        });'
}
COUNT_BEFORE="$(job_count)"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$RECORD_ID/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "re-approve returned $CODE"
sleep 3
STATUS="$(curl -sf "$BASE/memory/status")"
COUNT_AFTER="$(printf '%s' "$STATUS" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        process.stdout.write(String((JSON.parse(d).jobs || {}).completed || 0));
    });')"
[ "$COUNT_AFTER" = "$COUNT_BEFORE" ] || fail "job counts changed after re-approve: before=$COUNT_BEFORE after=$COUNT_AFTER: $STATUS"
printf '%s' "$STATUS" | grep -q '"pending":[1-9]' && fail "a second job appeared after re-approve: $STATUS"
BANK2="$(curl -sf "$BASE/memory/bank?limit=50")"
TOTAL2="$(printf '%s' "$BANK2" | sed -n 's/.*"total":\([0-9]*\).*/\1/p')"
[ "$TOTAL2" = "1" ] || fail "duplicate unit after re-approve: $TOTAL2"
echo "idempotent"

echo
echo "MEMORY_RECORD_APPROVAL_TEST_PASSED"
