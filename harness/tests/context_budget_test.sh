#!/usr/bin/env bash
# context_budget_test.sh - a session whose transcript outgrows the provider
# window must not hit the provider's 400: the daemon token-estimates the
# serialized request and, when it would overflow, compacts the oldest turns
# into the session checkpoint (summarized, never trimmed) and rebuilds the
# request. The mock provider enforces the limit the way a real one does.
#
# Usage: harness/tests/context_budget_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8962
DAEMON_PORT=8963
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
# provider.context_window set below: 16384. The daemon's request budget is
# window - 2048 (reply) - 1024 (template) = 13312 tokens, estimated at ~2
# bytes/token in this test (no title model configured), so ~26600 bytes.
# 20 turns of 3000 chars is ~28000 tokens un-compacted (over the budget), so
# the guard fires. With no aux model the first compaction keeps the tail
# verbatim (still over budget), so the guard re-compresses and absorbs the
# tail: the final request is system + checkpoint only (~10000 tokens,
# ~20000 bytes). The mock 400s anything over 28000 bytes: just above the
# daemon's budget (26624 bytes), so it accepts every request the daemon
# correctly sends and rejects an un-compacted oversized one (~56000 bytes).
MAX_REQUEST_BYTES=28000
MESSAGE_COUNT=20

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
mkdir -p da
REQUEST_LOG="$WORKDIR/requests.jsonl"
FILLER="$(printf 'fillerword%.0s' $(seq 1 300))"  # 3000 chars

MOCK_PORT="$MOCK_PORT" REQUEST_LOG="$REQUEST_LOG" \
    MAX_REQUEST_BYTES="$MAX_REQUEST_BYTES" \
    "$NODE" "$ROOT/harness/tests/mock_budget_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model \
    --auxiliary "http://127.0.0.1:$MOCK_PORT/aux/v1" --auxiliary-model mock-aux \
    --auxiliary-no-thinking \
    --tool-timeout-ms 120000 ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; echo "--- daemon.log ---" >&2; tail -40 "$WORKDIR/daemon.log" >&2 2>/dev/null || true; exit 1; }

wait_health() {
    for _ in $(seq 1 50); do
        if curl -sf "$BASE/health" > /dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
wait_health || fail "daemon did not come up"
echo "ok"

echo "== small provider window =="
CODE="$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/settings" \
    -H 'Content-Type: application/json' -d '{"provider.context_window": 16384}')"
[ "$CODE" = "200" ] || fail "setting provider.context_window failed (got $CODE)"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

completed_runs() {
    curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            process.stdout.write(String(evs.filter((e) => e.event_type === "run.completed").length));
        });'
}

echo "== $MESSAGE_COUNT long turns =="
for i in $(seq 1 $MESSAGE_COUNT); do
    MARKER="$(printf 'BUDGET MARKER %04d' "$i")"
    RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$MARKER $FILLER\"}" | \
        sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$RUN" ] || fail "run $i did not start"
    for _ in $(seq 1 150); do
        N="$(completed_runs)"
        [ "${N:-0}" -ge "$i" ] && break
        sleep 0.2
    done
    N="$(completed_runs)"
    [ "${N:-0}" -ge "$i" ] || fail "run $i did not complete (completed=$N)"
done
echo "ok"

echo "== assertions =="
curl -sf "$BASE/sessions/$SID" > session.json
grep -q '"context_mode":"summary"' session.json || \
    fail "session did not end in summary mode"
grep -q 'context.compaction_started' session.json || \
    fail "no context.compaction_started event"
grep -q 'context.compacted' session.json || \
    fail "no context.compacted event"

LAST_REQ="$("$NODE" -e '
const fs = require("fs");
const lines = fs.readFileSync(process.argv[1], "utf8").trim().split(/\r?\n/).filter(Boolean);
if (!lines.length) process.exit(1);
const rows = lines.map((line) => JSON.parse(line)).filter((r) => r.endpoint === "primary");
if (!rows.length) process.exit(1);
const last = rows[rows.length - 1];
process.stdout.write(`${rows.length} ${last.request_bytes} ${last.status}`);
' "$REQUEST_LOG")" || fail "provider did not record requests"
set -- $LAST_REQ
[ "$1" -ge "$MESSAGE_COUNT" ] || fail "expected $MESSAGE_COUNT provider calls, got $1"
[ "$2" -lt "$MAX_REQUEST_BYTES" ] || fail "final request crossed the provider limit: $2 bytes"
[ "$3" = "200" ] || fail "final request was rejected by the provider (status $3)"

# The final request must carry the compact checkpoint, the latest turn, and
# not the oldest turn (which was folded into the checkpoint).
curl -sf "$BASE/sessions/$SID" > /dev/null 2>&1
"$NODE" -e '
const fs = require("fs");
const lines = fs.readFileSync(process.argv[1], "utf8").trim().split(/\r?\n/).filter(Boolean);
const rows = lines.map((line) => JSON.parse(line)).filter((r) => r.endpoint === "primary");
const last = rows[rows.length - 1];
const body = last.body || "";
const checks = [
  ["has compact checkpoint", body.includes("[COMPACT CONTEXT]")],
  ["has latest turn", body.includes("BUDGET MARKER 0020")],
  ["oldest turn compacted away", !body.includes("BUDGET MARKER 0001")],
];
let bad = 0;
for (const [name, ok] of checks) {
  console.log((ok ? "ok: " : "BAD: ") + name);
  if (!ok) bad = 1;
}
process.exit(bad);
' "$REQUEST_LOG" || fail "final request shape assertions failed"

completed_runs_for() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            process.stdout.write(String(evs.filter((e) => e.event_type === "run.completed").length));
        });'
}

session_prompt() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            process.stdout.write(JSON.parse(d).context_prompt || "");
        });'
}

# Post one turn and wait for it to complete, answering the compaction prompt
# (aux is down for the whole scenario, so a run can park on it). Turns are
# posted sequentially: the daemon's queued-run re-anchoring has a leaf race
# that this test does not want to depend on.
run_with_decision() {
    local sid="$1" decision="$2" count="$3" i
    for i in $(seq 1 $count); do
        local MARKER RUN
        MARKER="$(printf 'FALL MARKER %04d' "$i")"
        RUN="$(curl -sf -X POST "$BASE/sessions/$sid/runs" -H 'Content-Type: application/json' \
            -d "{\"session_id\":\"$sid\",\"message\":\"$MARKER $FILLER\"}" | \
            sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
        [ -n "$RUN" ] || return 1
        for _ in $(seq 1 600); do
            N="$(completed_runs_for "$sid")"
            [ "${N:-0}" -ge "$i" ] && break
            P="$(session_prompt "$sid")"
            if [ -n "$P" ]; then
                CODE="$(curl -s -o /dev/null -w "%{http_code}" -X POST \
                    "$BASE/sessions/$sid/context/decision" \
                    -H 'Content-Type: application/json' \
                    -d "{\"decision\":\"$decision\"}")"
                [ "$CODE" = "200" ] || return 1
            fi
            sleep 0.2
        done
        N="$(completed_runs_for "$sid")"
        [ "${N:-0}" -ge "$i" ] || return 1
    done
    return 0
}

# ── fallback scenarios: the auxiliary model is down, so the daemon parks the
# compaction on the operator's prompt (primary / deterministic / retry). ──

echo "== aux down: primary fallback =="
CODE="$(curl -s -o /dev/null -w "%{http_code}" -X POST \
    "http://127.0.0.1:$MOCK_PORT/control/aux-fail" \
    -H 'Content-Type: application/json' -d '{"fail":true}')"
[ "$CODE" = "200" ] || fail "toggling aux failure failed (got $CODE)"

SID2="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no session id (scenario 2)"
FALL_TURNS=12
run_with_decision "$SID2" "primary" $FALL_TURNS || \
    fail "runs did not complete with primary decisions (scenario 2)"

curl -sf "$BASE/sessions/$SID2" > session2.json
grep -q '"context_mode":"summary"' session2.json || \
    fail "scenario 2 session did not end in summary mode"
grep -q 'context.summary_prompt' session2.json || \
    fail "no context.summary_prompt event (scenario 2)"
grep -q 'context.summary_resolved' session2.json || \
    fail "no context.summary_resolved event (scenario 2)"
grep -q '"decision":"primary"' session2.json || \
    fail "resolved decision is not primary (scenario 2)"
grep -q 'context.compacted' session2.json || \
    fail "no context.compacted event (scenario 2)"

# The primary channel must have carried the summary call: a primary request
# with the quoted source transcript, under the window limit.
SUMMARY_CALLS="$("$NODE" -e '
const fs = require("fs");
const lines = fs.readFileSync(process.argv[1], "utf8").trim().split(/\r?\n/).filter(Boolean);
const rows = lines.map((line) => JSON.parse(line)).filter((r) =>
  r.endpoint === "primary" && r.body.includes("BEGIN QUOTED SOURCE TRANSCRIPT"));
process.stdout.write(String(rows.length));
' "$REQUEST_LOG")"
[ "$SUMMARY_CALLS" -ge 1 ] || fail "no primary summary call was recorded (scenario 2)"
echo "ok"

echo "== aux down: deterministic fallback =="
SID3="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID3" ] || fail "no session id (scenario 3)"
run_with_decision "$SID3" "deterministic" $FALL_TURNS || \
    fail "runs did not complete with deterministic decisions (scenario 3)"

curl -sf "$BASE/sessions/$SID3" > session3.json
grep -q '"context_mode":"summary"' session3.json || \
    fail "scenario 3 session did not end in summary mode"
grep -q 'context.fallback' session3.json || \
    fail "no context.fallback event (scenario 3)"
echo "ok"

# ── scenario 4: auto-compact circuit breaker ──────────────────────────────
# Every turn overflows the window, but the first three chains are too short
# to split (< 4 messages: failed runs persist only their user message), so
# the budget compaction fails structurally three times in a row. The breaker
# must then open: run 4's chain (4 messages) IS splittable, but the guard
# skips the compaction entirely and lets the provider's 400 be the honest
# failure. Without the breaker, run 4 would compact (aux is healthy) and the
# oldest turn would be folded into the checkpoint.
#
# Asserts:
#   - all 5 runs fail (one provider 400 each; 400 is not retryable)
#   - daemon.log reaches "consecutive failures 3" and then "circuit open"
#   - the session ledger has NO context.compaction_started event at all
#   - run 4's provider request still carries the oldest turn (marker 1)
echo "== breaker: 3 structural failures open the circuit =="
SID4="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID4" ] || fail "no session id (scenario 4)"
BIG="$(printf 'bigword%.0s' $(seq 1 2800))"  # 28000 chars

finished_runs_for() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            process.stdout.write(String(evs.filter(
              (e) => e.event_type === "run.completed" ||
                     e.event_type === "run.failed").length));
        });'
}

for i in 1 2 3 4 5; do
    RUN="$(curl -sf -X POST "$BASE/sessions/$SID4/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID4\",\"message\":\"BREAKER MARKER $i $BIG\"}" | \
        sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$RUN" ] || fail "breaker run $i did not start"
    for _ in $(seq 1 150); do
        N="$(finished_runs_for "$SID4")"
        [ "${N:-0}" -ge "$i" ] && break
        sleep 0.2
    done
    N="$(finished_runs_for "$SID4")"
    [ "${N:-0}" -ge "$i" ] || fail "breaker run $i did not finish (finished=$N)"
done

curl -sf "$BASE/sessions/$SID4" > session4.json
FAILED4="$(grep -o '"event_type":"run.failed"' session4.json | wc -l | tr -d ' ')"
[ "$FAILED4" = "5" ] || fail "expected 5 failed runs (scenario 4), got $FAILED4"
grep -q 'consecutive failures 3' daemon.log || \
    fail "breaker never reached 3 consecutive failures (daemon.log)"
grep -q 'compaction circuit open' daemon.log || \
    fail "circuit did not open on run 4 (daemon.log)"
grep -q 'context.compaction_started' session4.json && \
    fail "compaction was attempted after the circuit opened (scenario 4)"

# Scenarios 1-3 keep every request under the provider limit, so every 400 in
# the log belongs to scenario 4: exactly one per run (400 is not retryable).
PRIMARY_400="$(grep -c '"status":400' "$REQUEST_LOG" 2>/dev/null || true)"
[ "$PRIMARY_400" = "5" ] || fail "expected 5 provider 400s (scenario 4), got $PRIMARY_400"

# Run 4's request must still carry the oldest turn: without the breaker the
# 4-message chain would have compacted and marker 1 would be folded away.
"$NODE" -e '
const fs = require("fs");
const lines = fs.readFileSync(process.argv[1], "utf8").trim().split(/\r?\n/).filter(Boolean);
const rows = lines.map((line) => JSON.parse(line)).filter((r) =>
  r.endpoint === "primary" && r.body.includes("BREAKER MARKER 4"));
if (!rows.length) process.exit(1);
process.exit(rows[0].body.includes("BREAKER MARKER 1") ? 0 : 1);
' "$REQUEST_LOG" || fail "run 4 request did not carry the oldest turn (scenario 4)"
echo "ok"

echo
echo "final request: $2 bytes ($1 provider calls)"
echo "CONTEXT_BUDGET_TEST_PASSED"
