#!/usr/bin/env bash
# End-to-end check for queued→steer promotion: an operator can
# escalate a queued follow-up into a live mid-turn steer while the run is
# still open - the active run continues with the queued text in context
# instead of ending, the promoted run cancels WITHOUT retracting its
# message, and the transcript keeps one receive-order parent chain.
#
# Mock script (one run, three provider calls):
#   1. terminal "touch a.txt" -> gate; approved once
#   2. terminal "touch b.txt" -> gate (still parked when the promote lands:
#      the delivery ABORTS it - zap semantics, provenance steer-abort -
#      scenario B covers that explicitly)
#   3. text "ok" (after the promote + abort)
#
# Asserts:
#   - the parked second gate is aborted by the delivery (deny-flavored,
#     rule steer-abort; tool.completed error=aborted) - no operator decision
#   - the promoted run ledgeres run.cancelled and its message row survives
#   - run.steered_live provenance exists
#   - the transcript's parent chain is receive-order:
#     prompt -> promoted steer -> answer
#   - run.completed (the turn finished with the steer incorporated)
#
# Usage: harness/tests/steer_live_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9061
DAEMON_PORT=9062
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

CALLS='[{"name":"terminal","arguments":{"command":"touch a.txt"}},{"name":"terminal","arguments":{"command":"touch b.txt"}},{"text":"ok"}]'

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

CALLS="$CALLS" MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_spill_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

# pending_approval <sid> -> the newest STILL-PENDING approval's id (""
# when none). Requests are persistent ledger events, so resolved ones must
# be excluded by their approval.resolved counterpart.
pending_approval() {
    curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const events = s.events || [];
            const resolved = new Set(events
                .filter((e) => e.event_type === "approval.resolved" || e.event_type === "approval.auto")
                .map((e) => String(e.payload.approval_id || e.payload.name || ""))
                .filter(Boolean));
            const pending = events
                .filter((e) => e.event_type === "approval.requested")
                .filter((e) => !resolved.has(String(e.payload.approval_id || "")));
            process.stdout.write(pending.length ? String(pending[pending.length - 1].payload.approval_id || "") : "");
        });'
}

approve() {
    local sid="$1" run="$2" scope="$3"
    local apr code
    for _ in $(seq 1 250); do
        apr="$(pending_approval "$sid")"
        [ -n "$apr" ] && break
        sleep 0.2
    done
    [ -n "$apr" ] || return 1
    code="$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/runs/$run/commands" \
        -H 'Content-Type: application/json' \
        -d "{\"command\":\"approve\",\"approval_id\":\"$apr\",\"scope\":\"$scope\"}")"
    [ "$code" = "200" ] || return 1
    return 0
}

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
echo "$SID"

echo "== run 1: two gated touches, queue the steer mid-loop =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"touch both\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"

approve "$SID" "$RUN1" "once" || fail "the first touch's approval failed"

# Park on the second gate BEFORE the promote: if the delivery lands while
# the run is still streaming call 2, the gate opens after the steer and the
# run hangs on an approval nobody owes (the race scenario B's park-first
# wait exists to avoid). Same wait here keeps scenario A deterministic.
PARKED=""
for _ in $(seq 1 250); do
    if [ -n "$(pending_approval "$SID")" ]; then PARKED=1; break; fi
    sleep 0.2
done
[ -n "$PARKED" ] || fail "the second gate never opened"

echo "== queue run 2 (the future steer text) =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"also mention the created files by name in your answer\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not queue"

echo "== promote run 2's message into run 1 while its second gate is parked =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RUN1/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"steer_live\",\"queued_run_id\":\"$RUN2\"}")"
[ "$CODE" = "200" ] || fail "steer_live was not accepted (got $CODE)"
# The delivery aborts the parked gate (deny-flavored); no second approve is
# owed. The run continues and completes with the promoted text in context.

DONE=""
for _ in $(seq 1 400); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run 1 did not complete (see $WORKDIR/daemon.log)"
echo "run 1 completed"

SNAP="$(curl -sf "$BASE/sessions/$SID")"

echo "== assert: aborted parked gate, promotion provenance, run 2 cancelled, message survives =="
printf '%s' "$SNAP" | grep -q '"rule_id":"steer-abort"' \
    || fail "the parked gate was not aborted by the delivery (no steer-abort)"
printf '%s' "$SNAP" | grep -q '"error":"aborted"' \
    || fail "the aborted call's tool.completed is not error=aborted"
OPRULES="$(printf '%s' "$SNAP" | grep -o '"rule_id":"operator"' | wc -l)"
[ "$OPRULES" = "1" ] \
    || fail "the operator decided a gate beyond the first approved one ($OPRULES)"
printf '%s' "$SNAP" | grep -q '"event_type":"run.steered_live"' \
    || fail "no run.steered_live provenance event"
printf '%s' "$SNAP" | grep -q '"event_type":"run.cancelled"' \
    || fail "run 2 never cancelled"

echo "== assert: the transcript chain is receive-order =="
printf '%s' "$SNAP" | "$NODE" -e '
let d = "";
process.stdin.on("data", (c) => { d += c; });
process.stdin.on("end", () => {
    const s = JSON.parse(d);
    const msgs = s.messages || [];
    const prompt = msgs.find((m) => m.role === "user" && m.content === "touch both");
    const steer = msgs.find((m) => m.role === "user" && m.content.includes("also mention the created files"));
    const answer = msgs.find((m) => m.role === "assistant");
    if (!prompt || !steer || !answer) { console.error("FAIL: transcript rows missing"); process.exit(1); }
    if (steer.parent_id !== prompt.id) { console.error("FAIL: steer parent is not the prompt (" + steer.parent_id + " vs " + prompt.id + ")"); process.exit(1); }
    if (answer.parent_id !== steer.id) { console.error("FAIL: answer parent is not the promoted steer (" + answer.parent_id + " vs " + steer.id + ")"); process.exit(1); }
    if (steer.is_active !== true || answer.is_active !== true) { console.error("FAIL: promoted chain not active"); process.exit(1); }
});' || fail "the receive-order chain assertion failed"
echo "chain ok"

echo "STEER_LIVE_TEST_PASSED"

# ---- scenario B: the delivery aborts a parked gate (operator request) ----
# A second daemon on its own ports: same provider script, but the second
# gate is never decided - the promote itself must abort it.
MOCK_PORT=9063
DAEMON_PORT=9064
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

CALLS="$CALLS" MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_spill_provider.mjs" > mock2.log 2>&1 &
MPID2=$!
trap 'kill "$DPID" "$MPID" "$DPID2" "$MPID2" 2>/dev/null || true' EXIT
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon2.log 2>&1 &
DPID2=$!
sleep 1.5
curl -sf "$BASE/health" > /dev/null || fail "daemon 2 did not come up"

SID2="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no session id (daemon 2)"

RUN1="$(curl -sf -X POST "$BASE/sessions/$SID2/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID2\",\"message\":\"touch both\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start (daemon 2)"

approve "$SID2" "$RUN1" "once" || fail "the first touch's approval failed (daemon 2)"

# Park on the undecided second gate FIRST - the operator zaps while the
# approval card is open, which is exactly the hang this scenario covers.
RUN1_PARKED=""
for _ in $(seq 1 250); do
    if [ -n "$(pending_approval "$SID2")" ]; then RUN1_PARKED=1; break; fi
    sleep 0.2
done
[ -n "$RUN1_PARKED" ] || fail "the second gate never opened"

RUN2="$(curl -sf -X POST "$BASE/sessions/$SID2/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID2\",\"message\":\"also mention the created files by name in your answer\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not queue (daemon 2)"

# The promote must land while the run is parked on the undecided second
# gate; the handler aborts that gate immediately, so one attempt suffices
# (poll anyway for robustness).
CODE=""
for _ in $(seq 1 250); do
    CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RUN1/commands" \
        -H 'Content-Type: application/json' \
        -d "{\"command\":\"steer_live\",\"queued_run_id\":\"$RUN2\"}")"
    [ "$CODE" = "200" ] && break
    sleep 0.2
done
[ "$CODE" = "200" ] || fail "steer_live was not accepted (got $CODE)"

DONE=""
for _ in $(seq 1 400); do
    if curl -sf "$BASE/sessions/$SID2" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run 1 did not complete after the gate abort (see $WORKDIR/daemon2.log)"
echo "scenario B: run completed after abort"

SNAP2="$(curl -sf "$BASE/sessions/$SID2")"
printf '%s' "$SNAP2" | grep -q '"rule_id":"steer-abort"' \
    || fail "the aborted gate has no steer-abort provenance"
printf '%s' "$SNAP2" | grep -q '"error":"aborted"' \
    || fail "the aborted call's tool.completed is not error=aborted"
OPRULES="$(printf '%s' "$SNAP2" | grep -o '"rule_id":"operator"' | wc -l)"
[ "$OPRULES" = "1" ] \
    || fail "the operator decided a gate beyond the first approved one ($OPRULES)"
printf '%s' "$SNAP2" | grep -q '"event_type":"run.steered_live"' \
    || fail "no run.steered_live provenance after the abort"
printf '%s' "$SNAP2" | grep -q '"event_type":"run.cancelled"' \
    || fail "run 2 never cancelled after the abort"
printf '%s' "$SNAP2" | "$NODE" -e '
let d = "";
process.stdin.on("data", (c) => { d += c; });
process.stdin.on("end", () => {
    const s = JSON.parse(d);
    const msgs = s.messages || [];
    const steer = msgs.find((m) => m.role === "user" && m.content.includes("also mention the created files"));
    const answers = msgs.filter((m) => m.role === "assistant" && m.is_active);
    if (!steer) { console.error("FAIL: promoted steer missing (daemon 2)"); process.exit(1); }
    if (answers.length === 0) { console.error("FAIL: no active assistant answer (daemon 2)"); process.exit(1); }
    if (answers[0].parent_id !== steer.id) {
        console.error("FAIL: answer does not chain after the promoted steer (" + answers[0].parent_id + " vs " + steer.id + ")");
        process.exit(1);
    }
});' || fail "the abort chain assertion failed"
echo "scenario B: aborted gate, delivery landed, chain ok"

echo "STEER_LIVE_ABORT_TEST_PASSED"
