#!/usr/bin/env bash
# End-to-end check for the tiered "always" approval scope.
#
# A mock OpenAI-compatible provider answers each of its first TOOLS_N
# completions with a "terminal" tool call ("rm one.txt", a classifier-gated
# write), then plain text. Three phases:
#
#   A. approve the gate with scope "always" (persisted server-side)
#   B. daemon restart + fresh session: the tool must run ungated - the
#      always-allow set survives the restart in the store
#   C. remove the tool (POST /api/v1/approvals/always/remove): the gate is
#      back
#
# Asserts on the event ledger and the GET /api/v1/approvals projection.
#
# Usage: harness/tests/approval_always_test.sh [path-to-nyaxus_daemon]
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
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
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
    # A health answer from a stale daemon on the same port would make the
    # test assert against the wrong process, so the one we started must be
    # the one that is alive.
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon process died (port $DAEMON_PORT in use?) - see $WORKDIR/daemon.log"
}

start_mock() {
    MOCK_PORT="$MOCK_PORT" TOOLS_N="$1" "$NODE" "$ROOT/harness/tests/mock_tool_loop.mjs" > mock.log 2>&1 &
    MOCK_PID=$!
    for _ in $(seq 1 25); do
        kill -0 "$MOCK_PID" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$MOCK_PID" 2>/dev/null || fail "mock process died (port $MOCK_PORT in use?) - see $WORKDIR/mock.log"
}

count_events() {
    # $1 = event_type; counts events in the session ledger
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o "\"event_type\":\"$1\"" | wc -l | tr -d ' '
}

latest_approval_id() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o '"approval_id":"[^"]*"' | tail -1 \
        | sed 's/^"approval_id":"//; s/"$//'
}

wait_run_done() {
    # The tool timeout is 60s and a silent-spawn retry doubles that, so the
    # wait must cover a full retried attempt plus provider slack: 650 x 0.2s
    # = 130s. (Previously 75 x 0.2s = 15s, which failed the test on any
    # single hang even though the daemon-side retry would have succeeded.)
    for _ in $(seq 1 650); do
        if [ "$(count_events run.completed)" -ge 1 ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
start_mock 2
start_daemon
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "ok"

# ── Phase A: approve with "always" scope ────────────────────────────────────
echo "== phase A: fresh session, gate prompts, approve with always scope =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"use the terminal\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

AID=""
for _ in $(seq 1 75); do
    AID="$(latest_approval_id)"
    [ -n "$AID" ] && break
    sleep 0.2
done
[ -n "$AID" ] || fail "no approval.requested appeared"
curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"scope\":\"always\",\"approval_id\":\"$AID\"}" > /dev/null \
    || fail "approve command failed"
wait_run_done || fail "run did not complete (likely blocked on a second approval)"

APPROVALS="$(count_events approval.requested)"
TOOLS="$(count_events tool.completed)"
printf '%s' "$(curl -sf "$BASE/approvals")" | grep -q '"terminal"' \
    || fail "GET /approvals does not list terminal"
echo "approval.requested: $APPROVALS (want 1) · tool.completed: $TOOLS (want 2)"
[ "$APPROVALS" = "1" ] || fail "expected exactly 1 approval.requested, got $APPROVALS"
[ "$TOOLS" = "2" ] || fail "expected 2 tool.completed, got $TOOLS"

# ── Phase B: restart, tool must stay ungated ────────────────────────────────
echo "== phase B: daemon restart, fresh session, no re-prompt =="
kill "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
kill "$MOCK_PID" 2>/dev/null || true
MOCK_PID=""
sleep 0.5
start_mock 2
start_daemon

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id (phase B)"
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"use the terminal\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run did not start (phase B)"
wait_run_done || fail "run did not complete (phase B: likely blocked on an approval)"

APPROVALS="$(count_events approval.requested)"
TOOLS="$(count_events tool.completed)"
echo "approval.requested: $APPROVALS (want 0) · tool.completed: $TOOLS (want 2)"
[ "$APPROVALS" = "0" ] || fail "the always-allow set did not survive the restart (got $APPROVALS prompts)"
[ "$TOOLS" = "2" ] || fail "expected 2 tool.completed, got $TOOLS"

# ── Phase C: remove the tool, the gate is back ──────────────────────────────
echo "== phase C: remove the tool, the gate returns =="
kill "$MOCK_PID" 2>/dev/null || true
MOCK_PID=""
start_mock 1
curl -sf -X POST "$BASE/approvals/always/remove" -H 'Content-Type: application/json' \
    -d '{"tool":"terminal"}' > /dev/null || fail "remove command failed"
printf '%s' "$(curl -sf "$BASE/approvals")" | grep -q '"terminal"' \
    && fail "GET /approvals still lists terminal after removal"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id (phase C)"
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"use the terminal\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "run did not start (phase C)"

AID=""
for _ in $(seq 1 75); do
    AID="$(latest_approval_id)"
    [ -n "$AID" ] && break
    sleep 0.2
done
[ -n "$AID" ] || fail "the gate did not come back after removal"
curl -sf -X POST "$BASE/runs/$RUN3/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"deny\",\"approval_id\":\"$AID\"}" > /dev/null || fail "deny command failed"
wait_run_done || fail "run did not complete (phase C)"
APPROVALS="$(count_events approval.requested)"
echo "approval.requested: $APPROVALS (want 1)"
[ "$APPROVALS" = "1" ] || fail "expected exactly 1 approval.requested after removal, got $APPROVALS"

echo
echo "APPROVAL_ALWAYS_TEST_PASSED"
