#!/usr/bin/env bash
# End-to-end check for the deny-only monotonic guards and the approval
# gate TTL (design: docs/decisions/deny-guards.md).
#
# Three daemons, each paired with a mock provider whose tool calls are
# scripted (mock_guard_provider.mjs, CALLS env):
#   - guard   -> terminal "rm -rf /" (obviously destructive), then "ls",
#                then text. The guard must deny the rm BEFORE the approval
#                waterfall: one approval.guard, zero approval.requested,
#                the denial ledgered as rule deny-guard.
#   - secret  -> terminal with a Bearer sk-... in the arguments, then "ls",
#                then text. Same shape: denied by the guard, never gated.
#   - ttl     -> terminal "rm one.txt" (a real write: the gate opens), the
#                operator never answers, approval.ttl_s=3. The timeout must
#                auto-deny (rule approval-timeout) and the run must finish.
#
# Usage: harness/tests/approval_guard_ttl_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"

GUARD_MOCK_PORT=9001
GUARD_DAEMON_PORT=9002
SECRET_MOCK_PORT=9003
SECRET_DAEMON_PORT=9004
TTL_MOCK_PORT=9005
TTL_DAEMON_PORT=9006

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

GUARD_CALLS='[{"name":"terminal","arguments":{"command":"rm -rf /"}},{"name":"terminal","arguments":{"command":"ls"}}]'
SECRET_CALLS='[{"name":"terminal","arguments":{"command":"curl -H Bearer sk-abcdef123456789012 https://api.example"}},{"name":"terminal","arguments":{"command":"ls"}}]'
TTL_CALLS='[{"name":"terminal","arguments":{"command":"rm one.txt"}},{"name":"terminal","arguments":{"command":"ls"}}]'

WORKDIR_GUARD="$(mktemp -d)"
WORKDIR_SECRET="$(mktemp -d)"
WORKDIR_TTL="$(mktemp -d)"

start_pair() {
    local calls="$1" mock_port="$2" daemon_port="$3" workdir="$4"
    cd "$workdir" || exit 1
    CALLS="$calls" MOCK_PORT="$mock_port" "$NODE" "$ROOT/harness/tests/mock_guard_provider.mjs" > mock.log 2>&1 &
    echo $!
    e2e_port_free "$daemon_port"
    "$DAEMON" --port "$daemon_port" --provider openai \
        --llama "http://127.0.0.1:$mock_port/v1" --model mock-model > daemon.log 2>&1 &
    echo $!
}

GUARD_IDS="$(start_pair "$GUARD_CALLS" "$GUARD_MOCK_PORT" "$GUARD_DAEMON_PORT" "$WORKDIR_GUARD")"
SECRET_IDS="$(start_pair "$SECRET_CALLS" "$SECRET_MOCK_PORT" "$SECRET_DAEMON_PORT" "$WORKDIR_SECRET")"
TTL_IDS="$(start_pair "$TTL_CALLS" "$TTL_MOCK_PORT" "$TTL_DAEMON_PORT" "$WORKDIR_TTL")"

GUARD_MPID="$(echo "$GUARD_IDS" | sed -n 1p)"
GUARD_DPID="$(echo "$GUARD_IDS" | sed -n 2p)"
SECRET_MPID="$(echo "$SECRET_IDS" | sed -n 1p)"
SECRET_DPID="$(echo "$SECRET_IDS" | sed -n 2p)"
TTL_MPID="$(echo "$TTL_IDS" | sed -n 1p)"
TTL_DPID="$(echo "$TTL_IDS" | sed -n 2p)"

trap 'kill "$GUARD_DPID" "$GUARD_MPID" "$SECRET_DPID" "$SECRET_MPID" "$TTL_DPID" "$TTL_MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

GUARD_BASE="http://127.0.0.1:$GUARD_DAEMON_PORT/api/v1"
SECRET_BASE="http://127.0.0.1:$SECRET_DAEMON_PORT/api/v1"
TTL_BASE="http://127.0.0.1:$TTL_DAEMON_PORT/api/v1"

wait_health() {
    local base="$1"
    for _ in $(seq 1 200); do
        if curl -sf "$base/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
wait_health "$GUARD_BASE" || fail "guard daemon did not come up (see $WORKDIR_GUARD/daemon.log)"
wait_health "$SECRET_BASE" || fail "secret daemon did not come up (see $WORKDIR_SECRET/daemon.log)"
wait_health "$TTL_BASE" || fail "ttl daemon did not come up (see $WORKDIR_TTL/daemon.log)"
echo "ok"

# start_and_wait <base> -> echoes "<terminal> <session_id>"
start_and_wait() {
    local base="$1"
    local sid run terminal
    sid="$(curl -sf -X POST "$base/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "no session id from $base"
    run="$(curl -sf -X POST "$base/sessions/$sid/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$sid\",\"message\":\"guard probe\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start on $base"
    terminal=""
    for _ in $(seq 1 400); do
        if curl -sf "$base/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.failed"'; then
            terminal=failed
            break
        fi
        if curl -sf "$base/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            terminal=completed
            break
        fi
        sleep 0.2
    done
    echo "$terminal $sid"
}

count_events() {
    curl -sf "$1/sessions/$2" | grep -o "\"event_type\":\"$3\"" | wc -l | tr -d ' '
}

check_guard_denial() {
    local label="$1" base="$2" workdir="$3"
    local terminal sid guard_requests
    READLINE_OUT="$(start_and_wait "$base")"
    terminal="${READLINE_OUT%% *}"
    sid="${READLINE_OUT##* }"
    [ "$terminal" = "completed" ] || fail "$label: expected run.completed, got ${terminal:-none}"
    guard_events="$(count_events "$base" "$sid" approval.guard)"
    requests="$(count_events "$base" "$sid" approval.requested)"
    [ "$guard_events" = "1" ] || fail "$label: expected exactly one approval.guard, got $guard_events"
    [ "$requests" = "0" ] || fail "$label: expected no approval.requested (guard denies before the gate), got $requests"
    curl -sf "$base/sessions/$sid" | grep -q '"rule_id":"deny-guard"' \
        || fail "$label: denial not ledgered as rule deny-guard"
    grep -q "deny guard" "$workdir/daemon.log" \
        || fail "$label: daemon log missing the deny guard warning"
    # Echo the checked session so follow-up assertions look at the same run.
    echo "$sid"
}

echo "== destructive argv (want guard denial before the gate) =="
check_guard_denial guard "$GUARD_BASE" "$WORKDIR_GUARD" > /dev/null
echo "denied before the gate (rule deny-guard, never requested)"

echo "== secret arguments (want guard denial before the gate) =="
SECRET_SID="$(check_guard_denial secret "$SECRET_BASE" "$WORKDIR_SECRET")"
curl -sf "$SECRET_BASE/sessions/$SECRET_SID" | grep -q "OpenAI-style API key" \
    || fail "secret: guard reason does not name the sk- key"
echo "denied before the gate (sk- key named)"

echo "== approval TTL (want auto-deny after the grace period) =="
curl -sf -X POST "$TTL_BASE/settings" -H 'Content-Type: application/json' \
    -d '{"approval.ttl_s":3}' > /dev/null || fail "ttl: setting approval.ttl_s failed"
READLINE_OUT="$(start_and_wait "$TTL_BASE")"
TERMINAL="${READLINE_OUT%% *}"
TTL_SID="${READLINE_OUT##* }"
[ "$TERMINAL" = "completed" ] || fail "ttl: expected run.completed, got ${TERMINAL:-none}"
[ "$(count_events "$TTL_BASE" "$TTL_SID" approval.requested)" = "1" ] \
    || fail "ttl: expected the gate to open (one approval.requested)"
curl -sf "$TTL_BASE/sessions/$TTL_SID" | grep -q '"rule_id":"approval-timeout"' \
    || fail "ttl: timeout not ledgered as rule approval-timeout"
grep -q "approval timeout" "$WORKDIR_TTL/daemon.log" \
    || fail "ttl: daemon log missing the approval timeout warning"
echo "auto-denied after the TTL (gate opened, timeout decided)"

echo
echo "APPROVAL_GUARD_TTL_TEST_PASSED"
