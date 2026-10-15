#!/usr/bin/env bash
# memory_dedup_test.sh - LLM semantic dedup on the extract path.
#
# Covers (two-phase dedup: fast candidate recall, then one batched LLM
# judgment):
#   - a pre-seeded existing unit is recalled as a candidate for a similar new
#     extract candidate
#   - the batch LLM judge decides per candidate (the mock decides by
#     candidate index, so no marker tokens in the text):
#       * index 0 -> skip: candidate discarded (skipped, not stored)
#       * index 1 -> merge: merged text stored as a new unit + a gated
#                    consolidation proposal (kind=dedup) to retire the
#                    superseded unit
#       * else    -> store: stored as-is (this also covers the store-all
#                    fallback shape: a candidate with no similar units is
#                    stored without a proposal)
#
# Usage: harness/tests/memory_dedup_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MAIN_PORT=9061
EXTRACT_PORT=9062
DAEMON_PORT=9063
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

# bank_unit_id <needle> [require-no-merged-prefix] -> unit id whose text
# contains <needle> (and, when the 2nd arg is set, does not start with MERGED:)
bank_unit_id() {
    curl -sf "$BASE/memory/bank?limit=200&state=all" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const units = (JSON.parse(d).units) || [];
            const noMerged = process.argv[2] === "nomerged";
            const unit = units.find((u) => {
                const t = u.text || "";
                if (!t.includes(process.argv[1])) return false;
                if (noMerged && t.includes("MERGED:")) return false;
                return true;
            });
            process.stdout.write(unit ? String(unit.id).replace(/^mem:/, "") : "");
        });' "$1" "${2:-}"
}

MOCK_PORT="$MAIN_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > main.log 2>&1 &
MAINSID=$!
MOCK_PORT="$EXTRACT_PORT" "$NODE" "$ROOT/harness/tests/mock_memory_dedup_provider.mjs" > extract.log 2>&1 &
EXTSID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_EXTRACT_URL="http://127.0.0.1:$EXTRACT_PORT/v1" \
NYAXUS_MEMORY_EXTRACT_MODEL=mock-dedup \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MAIN_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MAINSID" "$EXTSID" 2>/dev/null || true' EXIT
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

echo "== seed one existing unit (the merge target) via record approval =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
    -d "{\"text\":\"The beta protocol binds port 2222 on the gateway.\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
[ -n "$RECORD_ID" ] || fail "seed record was not created"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$RECORD_ID/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE"
SEED_BETA=""
for _ in $(seq 1 90); do
    SEED_BETA="$(bank_unit_id "beta protocol binds port 2222" nomerged)"
    [ -n "$SEED_BETA" ] && break
    sleep 0.5
done
[ -n "$SEED_BETA" ] || fail "seeded beta unit not found in bank"
echo "seeded unit $SEED_BETA"

echo "== one turn; the retain job drains through the (mock) extractor + dedup judge =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"The user prefers concise answers. The beta protocol listens on port 2222 at the gateway. The build pipeline uses the zig compiler.\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (see $WORKDIR/daemon.log)"
echo "run completed"

# The bank poll doubles as the drain wait: the zig-compiler unit is stored
# (no similar unit), so its presence means the retain job completed.
BANK=""
for _ in $(seq 1 90); do
    BANK="$(curl -sf "$BASE/memory/bank?limit=200&state=all")"
    if printf '%s' "$BANK" | grep -q "build pipeline uses the zig compiler"; then
        break
    fi
    sleep 0.5
done
printf '%s' "$BANK" | grep -q "build pipeline uses the zig compiler" || fail "retain job did not store the zig-compiler candidate: $BANK"
echo "retain job completed"

echo "== plain candidate stored as-is (store) =="
ZIG="$(bank_unit_id "build pipeline uses the zig compiler")"
[ -n "$ZIG" ] || fail "zig-compiler unit not stored"
echo "zig-compiler unit $ZIG"

echo "== merge candidate: merged unit stored + gated proposal to retire the seed =="
MERGED_BETA="$(bank_unit_id "MERGED: The beta protocol listens on port 2222 at the gateway")"
[ -n "$MERGED_BETA" ] || fail "merged beta unit not stored: $BANK"
[ "$MERGED_BETA" != "$SEED_BETA" ] || fail "merged unit should be a new unit, not the seed"
echo "merged unit $MERGED_BETA"

CONS="$(curl -sf "$BASE/memory/consolidations?status=pending")"
DEDUP_ROW="$(printf '%s' "$CONS" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const rows = (JSON.parse(d).consolidations) || (JSON.parse(d).items) || [];
        const row = rows.find((r) => r.kind === "dedup" &&
            String(r.newer_unit_id) === String(process.argv[1]) &&
            String(r.older_unit_id) === String(process.argv[2]));
        process.stdout.write(row ? "found" : "missing");
    });' "$MERGED_BETA" "$SEED_BETA")"
[ "$DEDUP_ROW" = "found" ] || fail "no kind=dedup proposal (older=$SEED_BETA newer=$MERGED_BETA): $CONS"
echo "dedup proposal present (older=$SEED_BETA -> newer=$MERGED_BETA)"

echo "== skip candidate discarded (not stored) =="
CONCISE="$(bank_unit_id "user prefers concise answers")"
[ -z "$CONCISE" ] || fail "concise-answers unit should have been skipped, but found $CONCISE"
echo "concise-answers candidate skipped"

echo
echo "MEMORY_DEDUP_TEST_PASSED"
