#!/usr/bin/env bash
# memory_sweep_test.sh - consolidation sweep (settled sweep decision 2026-09-11).
#
# Covers:
#   - recovery-on-start: the worker enqueues one sweep at startup (no-op
#     fixpoint while the bank is empty)
#   - idle debounce: a retain-quiet window enqueues one sweep that *proposes*
#     the near-duplicate pair; nothing is superseded until the operator
#     approves the candidate (a distinct unit in another session untouched)
#   - manual trigger: POST /memory/consolidate full-scans the whole active
#     bank (not just since-watermark) and is a fixpoint, because the decided
#     pair is never re-proposed and the rest is within the 24h cooldown
#   - memory.sweep_cosine_threshold is operator-tunable: a new pair under
#     the raised bar is not proposed (and the daemon logs the closest
#     below-threshold pair); once the bar is lowered the pair is proposed,
#     rejected (both stay active, never re-proposed), and a fresh pair is
#     still proposed and approved (older unit superseded)
#
# Usage: harness/tests/memory_sweep_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9005
DAEMON_PORT=9006
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

# last_sweep <field> -> the field of the newest sweep audit row ("" if none)
last_sweep() {
    curl -sf "$BASE/memory/status" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const sweep = ((JSON.parse(d).custom || {}).last_sweep) || {};
            const v = sweep[process.argv[1]];
            process.stdout.write(v === undefined || v === null ? "" : String(v));
        });' "$1"
}

# bank_unit <text-substring> -> "id|state|state_reason" ("" if not found)
bank_unit() {
    curl -sf "$BASE/memory/bank?limit=50&state=all" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const bank = JSON.parse(d);
            const unit = (bank.units || []).find((u) => (u.text || "").includes(process.argv[1]));
            if (!unit) return;
            process.stdout.write(String(unit.id).replace(/^mem:/, "") + "|" +
                (unit.state || "") + "|" + (unit.state_reason || ""));
        });' "$1"
}

# consolidation_id <text-substring> -> the pending candidate id of a pair
# containing a unit with that text ("" if none)
consolidation_id() {
    curl -sf "$BASE/memory/consolidations?status=pending" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const items = JSON.parse(d).consolidations || [];
            const item = items.find((c) => ((c.older_text || "").includes(process.argv[1])) ||
                                            ((c.newer_text || "").includes(process.argv[1])));
            if (item) process.stdout.write(String(item.id));
        });' "$1"
}

# drained_ok <min-completed> -> 1 when at least N jobs completed and none
# pending/sending (sweep jobs complete alongside retains, so the count is
# a lower bound, not exact).
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

seed_approved() {  # seed_approved <session-id> <text>
    local record_id code
    record_id="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
        -d "{\"text\":\"$2\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$1\"}" \
        | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
    [ -n "$record_id" ] || fail "record was not created"
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$record_id/approve" -d '{}')"
    [ "$code" = "200" ] || fail "approve returned $code"
}

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_SWEEP_IDLE_SECONDS=2 \
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
    if curl -sf "$BASE/memory/status" 2>/dev/null | grep -q '"ready":true'; then
        READY=1
        break
    fi
    sleep 0.5
done
[ "$READY" = "1" ] || fail "custom provider never became ready"
echo "ready"

echo "== recovery-on-start sweep (no-op on the empty bank) =="
RECOVERED=""
for _ in $(seq 1 60); do
    TRIGGER="$(last_sweep trigger)"
    if [ "$TRIGGER" = "recovery" ]; then
        RECOVERED=1
        break
    fi
    sleep 0.5
done
[ "$RECOVERED" = "1" ] || fail "no recovery sweep was recorded (daemon.log: $WORKDIR/daemon.log)"
[ "$(last_sweep fixpoint)" = "true" ] || fail "recovery sweep on an empty bank should be a fixpoint"
echo "recovery sweep ok"

echo "== seed a near-duplicate pair (session A) + a distinct unit (session B) =="
SID_A="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_A" ] || fail "no session id A"
SID_B="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_B" ] || fail "no session id B"
seed_approved "$SID_A" "The zephyr protocol listener binds port 4242 on the gateway."
seed_approved "$SID_A" "The zephyr protocol listener binds port 4242 on the gateway host."
seed_approved "$SID_B" "The quasar relay binds port 5555 on the edge."
DRAINED=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 3)" = "1" ]; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "retain jobs did not complete: $(curl -sf "$BASE/memory/status")"
echo "3 units retained"

echo "== idle sweep proposes the near-duplicate pair =="
SWEEPED=""
for _ in $(seq 1 60); do
    if [ "$(last_sweep trigger)" = "idle" ]; then
        SWEEPED=1
        break
    fi
    sleep 0.5
done
[ "$SWEEPED" = "1" ] || fail "no idle sweep was recorded after the retain-quiet window"
CHANGES="$(last_sweep changes)"
[ "${CHANGES:-0}" -ge 1 ] 2>/dev/null || fail "idle sweep proposed nothing (changes='${CHANGES:-0}')"
echo "idle sweep proposed $CHANGES candidate(s)"

# Proposal only: both near-duplicates (the bank is newest-first) must still
# be active until the operator approves the candidate.
PAIRS="$(curl -sf "$BASE/memory/bank?limit=50&state=all" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const bank = JSON.parse(d);
        const pair = (bank.units || []).filter((u) => (u.text || "").includes("zephyr protocol"));
        if (pair.length !== 2) { process.stdout.write("ERR:expected 2 units, got " + pair.length); return; }
        if (pair.some((u) => u.state !== "active")) {
            process.stdout.write("ERR:states=" + pair.map((u) => u.state).join(","));
            return;
        }
        process.stdout.write("OK");
    });')"
[ "$PAIRS" = "OK" ] || fail "proposed pair must stay untouched until approved: $PAIRS"
echo "pair proposed (both still active)"

# The approval gate: approving the candidate supersedes the older unit.
CAND="$(consolidation_id "zephyr protocol")"
[ -n "$CAND" ] || fail "no pending consolidation candidate for the zephyr pair"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidations/$CAND/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE"
PAIRS="$(curl -sf "$BASE/memory/bank?limit=50&state=all" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const bank = JSON.parse(d);
        const pair = (bank.units || []).filter((u) => (u.text || "").includes("zephyr protocol"));
        if (pair.length !== 2) { process.stdout.write("ERR:expected 2 units, got " + pair.length); return; }
        const superseded = pair.find((u) => u.state === "superseded");
        const active = pair.find((u) => u.state === "active");
        if (!superseded || !active) {
            process.stdout.write("ERR:states=" + pair.map((u) => u.state).join(","));
            return;
        }
        if (!(superseded.state_reason || "").includes("near-duplicate")) {
            process.stdout.write("ERR:no near-duplicate reason: " + superseded.state_reason);
            return;
        }
        process.stdout.write("OK");
    });')"
[ "$PAIRS" = "OK" ] || fail "approved pair not collapsed correctly: $PAIRS"
echo "approved: pair collapsed (one superseded, one active)"

DISTINCT="$(bank_unit "quasar relay")"
[ -n "$DISTINCT" ] || fail "distinct unit missing from the bank"
DISTINCT_STATE="$(printf '%s' "$DISTINCT" | cut -d'|' -f2)"
[ "$DISTINCT_STATE" = "active" ] || fail "distinct unit must stay active, got '$DISTINCT_STATE'"
echo "distinct unit untouched"

echo "== manual trigger full-scans the bank and is a fixpoint (cooldown) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidate" -d '{}')"
[ "$CODE" = "200" ] || fail "consolidate returned $CODE"
MANUAL=""
for _ in $(seq 1 60); do
    if [ "$(last_sweep trigger)" = "manual" ]; then
        MANUAL=1; break
    fi
    sleep 0.5
done
[ "$MANUAL" = "1" ] || fail "manual sweep was not recorded"
# Full scan: the distinct unit is selected (the decided pair is within the
# 24h cooldown and never re-proposed), so selected must be >= 1 even though
# the bank is unchanged.
SELECTED="$(last_sweep selected)"
[ "${SELECTED:-0}" -ge 1 ] 2>/dev/null || fail "manual sweep should full-scan the bank (selected='${SELECTED:-0}')"
[ "$(last_sweep fixpoint)" = "true" ] || fail "manual sweep should be a fixpoint"
[ "$(last_sweep changes)" = "0" ] || fail "manual sweep should propose nothing new"
echo "manual sweep ok (full scan, $SELECTED selected, fixpoint)"

echo "== threshold is operator-tunable (raised bar misses, lowered bar hits) =="
# Seed a fresh near-duplicate pair (session C) that no sweep has seen yet.
SID_C="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_C" ] || fail "no session id C"
seed_approved "$SID_C" "The aurora relay binds port 7777 on the spine."
seed_approved "$SID_C" "The aurora relay binds port 7777 on the spine node."
DRAINED2=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 5)" = "1" ]; then
        DRAINED2=1; break
    fi
    sleep 0.5
done
[ "$DRAINED2" = "1" ] || fail "retain jobs for the new pair did not complete"

# Raise the bar above the pair's similarity: the sweep must miss it and log
# the closest below-threshold pair.
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' -d '{"memory.sweep_cosine_threshold":0.9999}')"
[ "$CODE" = "200" ] || fail "could not raise the sweep threshold: $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidate" -d '{}')"
[ "$CODE" = "200" ] || fail "consolidate returned $CODE"
MANUAL2=""
for _ in $(seq 1 60); do
    if [ "$(last_sweep trigger)" = "manual" ] && [ "$(last_sweep changes)" = "0" ] && [ "$(last_sweep selected)" -ge 2 ] 2>/dev/null; then
        MANUAL2=1; break
    fi
    sleep 0.5
done
[ "$MANUAL2" = "1" ] || fail "raised-threshold sweep did not complete as a fixpoint"
grep -q "closest below threshold" daemon.log || fail "daemon did not log the closest below-threshold pair"
echo "raised bar: pair missed, closest pair logged"

# Lower the bar below the pair's similarity: the sweep must propose it.
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' -d '{"memory.sweep_cosine_threshold":0.90}')"
[ "$CODE" = "200" ] || fail "could not lower the sweep threshold: $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidate" -d '{}')"
[ "$CODE" = "200" ] || fail "consolidate returned $CODE"
# The proposal may land via the manual sweep or the idle debounce (whichever
# runs first); wait for the pending candidate itself.
AURORA=""
for _ in $(seq 1 60); do
    if [ -n "$(consolidation_id "aurora relay")" ]; then
        AURORA=1; break
    fi
    sleep 0.5
done
[ "$AURORA" = "1" ] || fail "lowered-threshold sweep did not propose the aurora pair"
CAND_C="$(consolidation_id "aurora relay")"
[ -n "$CAND_C" ] || fail "no pending candidate for the aurora pair"
# Rejecting keeps both units active and the pair is never re-proposed.
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidations/$CAND_C/reject" -d '{}')"
[ "$CODE" = "200" ] || fail "reject returned $CODE"
PAIRS_REJ="$(curl -sf "$BASE/memory/bank?limit=50&state=all" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const bank = JSON.parse(d);
        const pair = (bank.units || []).filter((u) => (u.text || "").includes("aurora relay"));
        if (pair.length !== 2) { process.stdout.write("ERR:expected 2 units, got " + pair.length); return; }
        if (pair.some((u) => u.state !== "active")) { process.stdout.write("ERR:states=" + pair.map((u) => u.state).join(",")); return; }
        process.stdout.write("OK");
    });')"
[ "$PAIRS_REJ" = "OK" ] || fail "rejected pair must stay active: $PAIRS_REJ"
echo "lowered bar: pair proposed, rejected (both stay active)"

# A rejected pair is never re-proposed: a fresh manual sweep must be a
# fixpoint (distinguished by its finished_at stamp).
BEFORE_SWEEP="$(last_sweep finished_at)"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidate" -d '{}')"
[ "$CODE" = "200" ] || fail "consolidate returned $CODE"
MANUAL4=""
for _ in $(seq 1 60); do
    NOW_SWEEP="$(last_sweep finished_at)"
    if [ -n "$NOW_SWEEP" ] && [ "$NOW_SWEEP" != "$BEFORE_SWEEP" ] &&
       [ "$(last_sweep trigger)" = "manual" ] && [ "$(last_sweep changes)" = "0" ] &&
       [ "$(last_sweep fixpoint)" = "true" ]; then
        MANUAL4=1; break
    fi
    sleep 0.5
done
[ "$MANUAL4" = "1" ] || fail "rejected pair was re-proposed (expected a fixpoint)"
echo "rejected pair not re-proposed"

# A fresh pair is still proposed and can be approved.
SID_D="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_D" ] || fail "no session id D"
seed_approved "$SID_D" "The borealis relay binds port 8888 on the trunk."
seed_approved "$SID_D" "The borealis relay binds port 8888 on the trunk node."
DRAINED3=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 7)" = "1" ]; then
        DRAINED3=1; break
    fi
    sleep 0.5
done
[ "$DRAINED3" = "1" ] || fail "retain jobs for the borealis pair did not complete"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidate" -d '{}')"
[ "$CODE" = "200" ] || fail "consolidate returned $CODE"
BOREALIS=""
for _ in $(seq 1 60); do
    if [ -n "$(consolidation_id "borealis relay")" ]; then
        BOREALIS=1; break
    fi
    sleep 0.5
done
[ "$BOREALIS" = "1" ] || fail "sweep did not propose the borealis pair"
CAND_D="$(consolidation_id "borealis relay")"
[ -n "$CAND_D" ] || fail "no pending candidate for the borealis pair"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidations/$CAND_D/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE"
PAIRS_D="$(curl -sf "$BASE/memory/bank?limit=50&state=all" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const bank = JSON.parse(d);
        const pair = (bank.units || []).filter((u) => (u.text || "").includes("borealis relay"));
        if (pair.length !== 2) { process.stdout.write("ERR:expected 2 units, got " + pair.length); return; }
        const superseded = pair.find((u) => u.state === "superseded");
        const active = pair.find((u) => u.state === "active");
        if (!superseded || !active) { process.stdout.write("ERR:states=" + pair.map((u) => u.state).join(",")); return; }
        if (!(superseded.state_reason || "").includes("near-duplicate")) {
            process.stdout.write("ERR:no near-duplicate reason: " + superseded.state_reason);
            return;
        }
        process.stdout.write("OK");
    });')"
[ "$PAIRS_D" = "OK" ] || fail "approved borealis pair not collapsed: $PAIRS_D"
echo "approved: borealis pair collapsed"

# Conflict detection (memory-compare-2026-10 item 3): same-topic texts with
# disjoint values -> a consolidation candidate with kind=conflict, riding
# the same approval queue.
echo "== conflict detection: same topic, disjoint values -> conflict candidate =="
# Restore the default sweep bar first: the previous phase lowered it to
# 0.90 and this pair's cosine is ~0.91, which the near-duplicate path
# would swallow. The conflict band is [0.55, threshold).
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' -d '{"memory.sweep_cosine_threshold":0.92}')"
[ "$CODE" = "200" ] || fail "could not restore the sweep threshold: $CODE"
SID_E="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_E" ] || fail "no session id E"
# Distinct-value texts: the hash-dedupe rejects any byte-identical version,
# so neither text may repeat a unit that already exists anywhere in the bank
# (session A's retired 4242 unit shares its text). Wait for both units to be
# visible in the bank before sweeping - the job-completed count is a lower
# bound that past sweeps can satisfy early.
seed_approved "$SID_E" "The zephyr protocol listener binds port 4343 on the gateway."
seed_approved "$SID_E" "Connections to the zephyr listener on the gateway are accepted on port 5566."
UNITS_E=""
for _ in $(seq 1 120); do
    BANK="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
    if printf '%s' "$BANK" | grep -q "port 4343" && \
       printf '%s' "$BANK" | grep -q "accepted on port 5566"; then
        UNITS_E=1
        break
    fi
    sleep 0.5
done
[ "$UNITS_E" = "1" ] || fail "conflicting pair units did not store: $BANK"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidate" -d '{}')"
[ "$CODE" = "200" ] || fail "consolidate returned $CODE"
# The proposal may land via the manual sweep or the idle debounce; wait for
# the pending candidate itself and require kind=conflict.
KIND_E=""
CONFLICT=""
for _ in $(seq 1 60); do
    if [ -n "$(consolidation_id "port 4343")" ]; then
        CONFLICT=1
        KIND_E="$(curl -sf "$BASE/memory/consolidations?status=pending" | grep -o '"kind":"[a-z_]*"' | head -1)"
        break
    fi
    sleep 0.5
done
[ "$CONFLICT" = "1" ] || fail "conflict pair not proposed"
[ "$KIND_E" = '"kind":"conflict"' ] || fail "conflict pair not proposed as kind=conflict (got '$KIND_E')"
CAND_E="$(consolidation_id "port 4343")"
[ -n "$CAND_E" ] || fail "no pending candidate for the conflict pair"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidations/$CAND_E/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE"
PAIRS_E="$(curl -sf "$BASE/memory/bank?limit=50&state=all" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const bank = JSON.parse(d);
        const pair = (bank.units || []).filter((u) => u.session_id === process.argv[1]);
        if (pair.length !== 2) { process.stdout.write("ERR:expected 2 units, got " + pair.length); return; }
        const dead = pair.find((u) => u.state === "superseded" && (u.state_reason || "").includes("conflicts with"));
        const live = pair.find((u) => u.state === "active");
        if (!dead || !live) { process.stdout.write("ERR:states=" + pair.map((u) => u.state).join(",")); return; }
        process.stdout.write("OK");
    });' "$SID_E")"
[ "$PAIRS_E" = "OK" ] || fail "approved conflict pair not collapsed: $PAIRS_E"
echo "conflict candidate proposed, approved: one superseded with a conflicts-with reason"

echo
echo "MEMORY_SWEEP_TEST_PASSED"
