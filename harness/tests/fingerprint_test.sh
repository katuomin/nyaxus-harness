#!/usr/bin/env bash
# End-to-end check for the fingerprint rulebook (tiered "always" scope).
#
# A mock OpenAI-compatible provider answers its first completion with a
# "terminal" tool call carrying a known command, then plain text. The
# command is chosen per phase (the mock is re-armed by restarting it):
#
#   A. gate "git commit -m x", approve with scope "always" -> the derived
#      fingerprint "git commit" is persisted (GET /approvals shows it)
#   B. daemon restart + fresh session: "git commit -m y" runs UNGATED -
#      the pattern matches (same subcommand, any args) and survives restart
#   C. "git push origin" gates again - a different subcommand does not
#      match the stored pattern
#
# The mock commands are classifier-gated (git writes), so the readonly-argv
# auto-approve never shortcuts the gate.
#
# Usage: harness/tests/fingerprint_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8971
DAEMON_PORT=8972
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PID=""
DAEMON_PID=""
cleanup() {
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
}
trap cleanup EXIT

start_daemon() {
    "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
    DAEMON_PID=$!
    for _ in $(seq 1 75); do
        curl -sf "$BASE/health" > /dev/null && break
        sleep 0.2
    done
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon process died (port $DAEMON_PORT in use?) - see $WORKDIR/daemon.log"
}

start_mock() {
    MOCK_PORT="$MOCK_PORT" TOOLS_N=1 CMD="$1" "$NODE" "$ROOT/harness/tests/mock_fingerprint_loop.mjs" > mock.log 2>&1 &
    MOCK_PID=$!
    for _ in $(seq 1 25); do
        kill -0 "$MOCK_PID" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$MOCK_PID" 2>/dev/null || fail "mock process died (port $MOCK_PORT in use?) - see $WORKDIR/mock.log"
}

count_events() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o "\"event_type\":\"$1\"" | wc -l | tr -d ' '
}

session_ledger() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null
}

latest_approval_id() {
    session_ledger \
        | grep -o '"approval_id":"[^"]*"' | tail -1 \
        | sed 's/^"approval_id":"//; s/"$//'
}

wait_run_done() {
    # The tool timeout is 60s and a silent-spawn retry doubles that, so the
    # wait must cover a full retried attempt plus provider slack.
    for _ in $(seq 1 650); do
        if [ "$(count_events run.completed)" -ge 1 ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

start_session() {
    SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$SID" ] || fail "no session id"
    RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"run it\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$RUN" ] || fail "run did not start"
}

wait_approval() {
    AID=""
    for _ in $(seq 1 150); do
        AID="$(latest_approval_id)"
        [ -n "$AID" ] && return 0
        sleep 0.2
    done
    return 1
}

echo "== health =="
start_mock "git commit -m x"
start_daemon
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "ok"

# ── Phase A: approve "git commit" with "always" scope ──────────────────────
echo "== phase A: gate git commit, approve with always scope =="
start_session
wait_approval || fail "no approval.requested appeared (git commit should gate)"
session_ledger | grep -q '"fingerprint":"git commit"' \
    || fail "approval.requested carries no fingerprint label"
session_ledger | grep -q '"pattern_allowable":"true"' \
    || fail "approval.requested does not mark the call pattern-able"
curl -sf -X POST "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"scope\":\"always\",\"approval_id\":\"$AID\"}" > /dev/null \
    || fail "approve command failed"
wait_run_done || fail "run did not complete (phase A)"

APPROVALS="$(count_events approval.requested)"
echo "approval.requested: $APPROVALS (want 1)"
[ "$APPROVALS" = "1" ] || fail "expected exactly 1 approval.requested, got $APPROVALS"
printf '%s' "$(curl -sf "$BASE/approvals")" | grep -q '"fingerprint":"git commit"' \
    || fail "GET /approvals does not show the stored fingerprint"

# ── Phase B: restart; same subcommand runs ungated ─────────────────────────
echo "== phase B: restart, git commit -m y runs ungated (pattern match) =="
kill "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
kill "$MOCK_PID" 2>/dev/null || true
MOCK_PID=""
sleep 0.5
start_mock "git commit -m y"
start_daemon

start_session
wait_run_done || fail "run did not complete (phase B: likely blocked on an approval)"
APPROVALS="$(count_events approval.requested)"
echo "approval.requested: $APPROVALS (want 0)"
[ "$APPROVALS" = "0" ] || fail "git commit -m y re-gated: the fingerprint did not match / survive the restart"
# The bypass is ledgered as structured provenance: the rule id names the
# fingerprint that let the call run.
session_ledger | grep -q '"rule_id":"always-allow:git commit"' \
    || fail "the always-allow bypass did not ledger its rule id"
session_ledger | grep -q '"risk_level":"medium"' \
    || fail "the structured decision is missing its risk classification"

# ── Phase C: a different subcommand gates again ────────────────────────────
echo "== phase C: git push origin gates again (subcommand mismatch) =="
kill "$MOCK_PID" 2>/dev/null || true
MOCK_PID=""
start_mock "git push origin"

start_session
wait_approval || fail "the gate did not come back for git push (pattern should not match)"
curl -sf -X POST "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"deny\",\"approval_id\":\"$AID\"}" > /dev/null || fail "deny command failed"
wait_run_done || fail "run did not complete (phase C)"
APPROVALS="$(count_events approval.requested)"
echo "approval.requested: $APPROVALS (want 1)"
[ "$APPROVALS" = "1" ] || fail "expected exactly 1 approval.requested for git push, got $APPROVALS"
session_ledger | grep -q '"rule_id":"operator"' \
    || fail "the manual denial did not ledger the operator rule id"

echo
echo "FINGERPRINT_TEST_PASSED"
