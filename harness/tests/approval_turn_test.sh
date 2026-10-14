#!/usr/bin/env bash
# End-to-end check for the turn-bound approval scope: a
# "turn"-scoped approval lets the same tool run ungated for the rest of
# that run, and the grant expires with the run - the same tool re-prompts
# in the next turn. The missing middle scope between allow-once and
# allow-session.
#
# Mock script (1-based by global completion count, across runs):
#   run 1: 1. terminal "touch one.txt"   -> gate; approved with scope=turn
#         2. terminal "touch two.txt"    -> ungated (turn grant,
#                                           approval.auto rule allow-turn)
#         3. text                        -> run completes
#   run 2: 4. terminal "touch three.txt" -> gate re-opens (grant did not
#                                           leak into the new turn)
#         5. text                        -> run completes
#
# Usage: harness/tests/approval_turn_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9011
DAEMON_PORT=9012
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

CALLS='[{"name":"terminal","arguments":{"command":"touch one.txt"}},{"name":"terminal","arguments":{"command":"touch two.txt"}},{"text":"done"},{"name":"terminal","arguments":{"command":"touch three.txt"}},{"text":"done"}]'

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

CALLS="$CALLS" MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_turn_scope_provider.mjs" > mock.log 2>&1 &
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

# latest_approval_id <sid> -> the newest pending approval.requested id
latest_approval_id() {
    curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const reqs = (s.events || []).filter((e) => e.event_type === "approval.requested");
            process.stdout.write(reqs.length ? String(reqs[reqs.length - 1].payload.approval_id || "") : "");
        });'
}

count_events() {
    curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            process.stdout.write(String((s.events || []).filter((e) => e.event_type === process.argv[1]).length));
        });' "$2"
}

wait_terminal() {
    local sid="$1"
    local _
    for _ in $(seq 1 150); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
echo "$SID"

echo "== run 1: approve the first touch with scope=turn =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"touch one\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"

APPROVAL1=""
for _ in $(seq 1 75); do
    APPROVAL1="$(latest_approval_id "$SID")"
    [ -n "$APPROVAL1" ] && break
    sleep 0.2
done
[ -n "$APPROVAL1" ] || fail "the first touch never opened a gate (see $WORKDIR/daemon.log)"

CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RUN1/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APPROVAL1\",\"scope\":\"turn\"}")"
[ "$CODE" = "200" ] || fail "turn approval was not accepted (got $CODE)"

wait_terminal "$SID" || fail "run 1 did not complete (see $WORKDIR/daemon.log)"
echo "run 1 completed"

echo "== assert run 1: one gate, second touch ungated via the turn grant =="
REQUESTS1="$(count_events "$SID" approval.requested)"
AUTO_TURN="$(count_events "$SID" approval.auto)"
COMPLETED_TOOLS="$(count_events "$SID" tool.completed)"
echo "approval.requested: $REQUESTS1 (want 1)"
echo "approval.auto: $AUTO_TURN (want 1)"
echo "tool.completed: $COMPLETED_TOOLS (want 2)"
[ "$REQUESTS1" = "1" ] || fail "expected exactly 1 gate in run 1, got $REQUESTS1"
[ "$COMPLETED_TOOLS" = "2" ] || fail "expected both touches to run, got $COMPLETED_TOOLS completed"
[ "$AUTO_TURN" = "1" ] || fail "expected the second touch to ride the turn grant, got $AUTO_TURN auto approvals"
curl -sf "$BASE/sessions/$SID" | grep -q '"rule_id":"allow-turn"' \
    || fail "the auto approval did not ledger rule allow-turn"
curl -sf "$BASE/sessions/$SID" | grep -q 'pre-approved for this turn' \
    || fail "the auto approval reason did not name the turn scope"

echo "== run 2: the same tool must re-prompt (grant expired with the run) =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"touch three\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not start"

APPROVAL2=""
for _ in $(seq 1 75); do
    APPROVAL2="$(latest_approval_id "$SID")"
    [ "$APPROVAL2" != "$APPROVAL1" ] && [ -n "$APPROVAL2" ] && break
    sleep 0.2
done
[ -n "$APPROVAL2" ] || fail "the turn grant leaked: touch three never re-opened a gate (see $WORKDIR/daemon.log)"
echo "gate re-opened: $APPROVAL2"

CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RUN2/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APPROVAL2\",\"scope\":\"once\"}")"
[ "$CODE" = "200" ] || fail "run 2 approval was not accepted (got $CODE)"
wait_terminal "$SID" || fail "run 2 did not complete (see $WORKDIR/daemon.log)"

REQUESTS2="$(count_events "$SID" approval.requested)"
echo "approval.requested total: $REQUESTS2 (want 2)"
[ "$REQUESTS2" = "2" ] || fail "expected 2 gates across both runs, got $REQUESTS2"

echo "APPROVAL_TURN_TEST_PASSED"
