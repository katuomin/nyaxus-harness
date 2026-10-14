#!/usr/bin/env bash
# End-to-end check for the consecutive-denial circuit breaker.
#
# A mock provider calls a gated terminal tool ("rm storm<N>.txt", distinct per
# request) on every request and never answers with text. The operator denies it
# three times in a row; the daemon's breaker must hard-stop the run (run.failed
# with the denial message) instead of grinding on a denial storm.
#
# Asserts on the event ledger:
#   - exactly 3 approval.requested (one per denied call)
#   - exactly 3 approval.resolved with decision deny
#   - run.failed whose payload names the denial storm
#   - no run.completed (the run was stopped, not finished)
#
# Usage: harness/tests/denial_breaker_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8981
DAEMON_PORT=8982
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

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_denial_storm_provider.mjs" > mock.log 2>&1 &
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

echo "== create session + start run (mock calls the terminal tool forever) =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"remove the storm file\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

wait_approvals() {
    local want="$1" count=""
    for _ in $(seq 1 75); do
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

# Deny the same call three times in a row.
for i in 1 2 3; do
    echo "== denial $i =="
    wait_approvals "$i" || fail "approval.requested #$i did not appear"
    AID="$(latest_approval_id)"
    [ -n "$AID" ] || fail "no approval id in the event payload"
    curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
        -d "{\"command\":\"deny\",\"approval_id\":\"$AID\"}" > /dev/null || fail "deny #$i failed"
    echo "denied"
done

echo "== breaker must stop the run (want run.failed, not run.completed) =="
TERMINAL=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.failed"'; then
        TERMINAL=failed
        break
    fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        TERMINAL=completed
        break
    fi
    sleep 0.2
done
[ "$TERMINAL" = "failed" ] || fail "expected run.failed after 3 denials, got ${TERMINAL:-none}"

SNAP="$(curl -sf "$BASE/sessions/$SID")"
REQUESTS="$(printf '%s' "$SNAP" | grep -o '"event_type":"approval.requested"' | wc -l | tr -d ' ')"
DENIES="$(printf '%s' "$SNAP" | grep -o '"event_type":"approval.resolved"[^}]*"deny"' | wc -l | tr -d ' ')"
printf '%s' "$SNAP" | grep -q 'denial storm' || fail "run.failed payload does not name the denial storm"
echo "approval.requested: $REQUESTS (want 3)"
echo "approval.resolved deny: $DENIES (want 3)"
[ "$REQUESTS" = "3" ] || fail "expected 3 approval.requested, got $REQUESTS"
[ "$DENIES" = "3" ] || fail "expected 3 deny decisions, got $DENIES"

echo
echo "DENIAL_BREAKER_TEST_PASSED"
