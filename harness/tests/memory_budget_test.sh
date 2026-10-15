#!/usr/bin/env bash
# memory_budget_test.sh - recall budget hardening:
# per-item truncation, total-budget drop counting, structured timeout.
#
# Covers:
#   - a unit longer than the per-item cap (512 tokens) comes back truncated
#     with a visible "[truncated]" marker, and the envelope reports
#     truncated=1
#   - a tight total budget drops the hits that do not fit, and the
#     envelope reports budget_dropped
#   - a 1ms deadline expires inside the recall and returns 200 with
#     timed_out=true (partial results, not a failure)
#
# Usage: harness/tests/memory_budget_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MAIN_PORT=9041
DAEMON_PORT=9042
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

MOCK_PORT="$MAIN_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > main.log 2>&1 &
MAINSID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MAIN_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MAINSID" 2>/dev/null || true' EXIT
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

# Seed one approved record (direct retain path, no auxiliary model).
seed_approved() {
    local text="$1"
    local record_id code
    record_id="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
        -d "{\"text\":\"$text\",\"type\":\"fact\",\"scope\":\"session\"}" \
        | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
    [ -n "$record_id" ] || fail "record was not created"
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$record_id/approve" -d '{}')"
    [ "$code" = "200" ] || fail "approve returned $code"
}

# wait for the retain outbox to drain (completed >= n, none pending/sending)
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

echo "== seed: one long unit (4096 chars, over the 512-token cap) + four short units =="
LONG_TEXT=""
# 80 x 50 chars = 4000: over the 512-token cap (~1000 tokens) but under the
# 4096-char retain limit.
for _ in $(seq 1 80); do LONG_TEXT="${LONG_TEXT}The mangrove beacon pulses amber over the harbor. "; done
[ ${#LONG_TEXT} -ge 3500 ] && [ ${#LONG_TEXT} -le 4096 ] || fail "long text out of range: ${#LONG_TEXT}"
seed_approved "$LONG_TEXT"
seed_approved "The zephyr relay binds port 4242 on the gateway."
seed_approved "The aurora relay keeps the guild ledger in a brass vault."
seed_approved "The lighthouse keeper logs every ship that passes at dawn."
seed_approved "The cartographer files tide tables under the north shelf."
DRAINED=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 5)" = "1" ]; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "retain jobs did not complete: $(curl -sf "$BASE/memory/status")"
echo "5 units seeded"

echo "== per-item cap: the long unit comes back truncated, marked, counted =="
RECALL="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"mangrove beacon harbor","max_tokens":2048}')"
CHECK="$(printf '%s' "$RECALL" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const r = JSON.parse(d);
        const hit = (r.results || []).find((x) => (x.text || "").includes("mangrove beacon"));
        process.stdout.write([
            hit ? "found" : "missing",
            hit && hit.text.endsWith("[truncated]") ? "marked" : "unmarked",
            hit && hit.text.length < 4000 ? "bounded" : "unbounded",
            r.truncated === 1 ? "counted" : "not-counted",
        ].join("/"));
    });')"
[ "$CHECK" = "found/marked/bounded/counted" ] || fail "per-item truncation wrong (want found/marked/bounded/counted, got $CHECK): $RECALL"
echo "long unit truncated + marked + counted"

echo "== total budget: tight max_tokens drops the hits that do not fit =="
RECALL2="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"relay vault keeper cartographer zephyr aurora lighthouse tide","max_tokens":20}')"
CHECK2="$(printf '%s' "$RECALL2" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const r = JSON.parse(d);
        const n = (r.results || []).length;
        process.stdout.write([
            n < 4 ? "bounded" : "unbounded",
            (r.budget_dropped || 0) >= 1 ? "dropped" : "none",
        ].join("/"));
    });')"
[ "$CHECK2" = "bounded/dropped" ] || fail "budget drop wrong (want bounded/dropped, got $CHECK2): $RECALL2"
echo "tight budget dropped the overflow, reported"

echo "== structured timeout: 1ms deadline returns 200 + timed_out, not a failure =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/recall" \
    -H 'Content-Type: application/json' -d '{"query":"mangrove beacon","timeout_ms":1}')"
[ "$CODE" = "200" ] || fail "timed-out recall returned $CODE, want 200"
RECALL3="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"mangrove beacon","timeout_ms":1}')"
printf '%s' "$RECALL3" | grep -q '"timed_out":true' || fail "timed_out missing: $RECALL3"
echo "timed-out recall is a structured partial result"

echo
echo "MEMORY_BUDGET_TEST_PASSED"
