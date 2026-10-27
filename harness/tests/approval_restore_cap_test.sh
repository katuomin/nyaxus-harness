#!/usr/bin/env bash
# approval_restore_cap_test.sh - the safe-restore invariant for privilege
# grants (the grant safe-restore invariant; audited external checklist).
#
# A session-scoped approval grant lives in the daemon's memory only. When
# the process restarts, the restored session must come back at the SAFE
# tier: the first gated call re-prompts, and no persisted state re-arms the
# grant. (The always-allow rule set is the explicit operator opt-in and its
# restart persistence is tested in approval_always_test.sh phase B.)
#
# Asserts:
#   - phase one: one gated call, approved with "session" scope, run completes
#   - daemon restart
#   - phase two in the same session: the same gated call re-prompts
#     (approval.requested count 2) - the grant did not survive the restart
#   - phase two completes after the fresh approval
#
# Usage: harness/tests/approval_restore_cap_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9088
DAEMON_PORT=9089
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_restore_cap_provider.mjs" > mock.log 2>&1 &
MPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
e2e_port_free "$DAEMON_PORT"

launch_daemon() {
    ( cd "$WORKDIR" && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > "$1" 2>&1 &
    DPID=$!
    for _ in $(seq 1 50); do
        curl -sf "$BASE/health" > /dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

launch_daemon daemon1.log || fail "daemon did not come up (see $WORKDIR/daemon1.log)"
e2e_daemon_up "$DPID"
echo "healthy"

echo "== phase one: gated call approved with session scope =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"use the terminal\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"

wait_approvals() {
    # $1 = minimum number of approval.requested events to wait for
    local want="$1" count=""
    for _ in $(seq 1 250); do
        count="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -o '"event_type":"approval.requested"' | wc -l | tr -d ' ')"
        if [ -n "$count" ] && [ "$count" -ge "$want" ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

latest_approval_id() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o '"approval_id":"[^"]*"' | tail -1 \
        | sed 's/^"approval_id":"//; s/"$//'
}

wait_run_done() {
    for _ in $(seq 1 250); do
        curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"' && return 0
        sleep 0.2
    done
    return 1
}

wait_approvals 1 || fail "phase one never requested the approval"
AID="$(latest_approval_id)"
[ -n "$AID" ] || fail "no approval id in the event payload"
curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"scope\":\"session\",\"approval_id\":\"$AID\"}" > /dev/null \
    || fail "the session-scope approve failed"
wait_run_done || fail "run 1 did not complete"

APPROVALS="$(curl -sf "$BASE/sessions/$SID" | grep -o '"event_type":"approval.requested"' | wc -l | tr -d ' ')"
[ "$APPROVALS" = "1" ] || fail "phase one approvals: want 1, got $APPROVALS"
echo "phase one ok (1 approval, session scope)"

echo "== restart the daemon (the mock keeps running) =="
kill "$DPID" 2>/dev/null
for _ in $(seq 1 50); do
    kill -0 "$DPID" 2>/dev/null || break
    sleep 0.2
done
kill -0 "$DPID" 2>/dev/null && fail "the old daemon never exited"
e2e_port_free "$DAEMON_PORT"
launch_daemon daemon2.log || fail "the restarted daemon did not come up (see $WORKDIR/daemon2.log)"
e2e_daemon_up "$DPID"
echo "restarted"

echo "== phase two: the same gated call MUST re-prompt =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"use the terminal again\"}" | sed -n 's/.*"run_id":"\([^\"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not start"
# The dangerous outcome is run 2 completing ungated (still 1 approval, a
# second run.completed): that would mean the session-scope grant silently
# survived the restart. The good outcome is a second approval.requested.
PHASE2=""
for _ in $(seq 1 250); do
    SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null)" || { sleep 0.2; continue; }
    if [ "$(printf '%s' "$SNAP" | grep -o '"event_type":"approval.requested"' | wc -l | tr -d ' ')" -ge 2 ]; then
        PHASE2="gated"
        break
    fi
    if [ "$(printf '%s' "$SNAP" | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')" -ge 2 ]; then
        PHASE2="ungated"
        break
    fi
    sleep 0.2
done
[ "$PHASE2" = "gated" ] \
    || fail "the session-scope grant did not re-prompt after the restart (outcome: $PHASE2)"
AID2="$(latest_approval_id)"
[ -n "$AID2" ] || fail "no second approval id"
curl -sf -X POST "$BASE/runs/$RUN2/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"scope\":\"once\",\"approval_id\":\"$AID2\"}" > /dev/null \
    || fail "the once-scope approve failed"
for _ in $(seq 1 250); do
    N="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')"
    [ "$N" -ge 2 ] && break
    sleep 0.2
done
N="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')"
[ "$N" -ge 2 ] || fail "run 2 did not complete"
echo "phase two ok (re-prompted after restart)"

echo "APPROVAL_RESTORE_CAP_TEST_PASSED"
