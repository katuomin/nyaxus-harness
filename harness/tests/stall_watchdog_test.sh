#!/usr/bin/env bash
# End-to-end check for the provider stall watchdog.
#
# Asserts (run.stall_timeout_s set to 3 via the settings API):
#   - midstream stall (one delta, then silence) -> run.stalled + run.failed
#     whose payload names the stall, with the partial text persisted
#   - pre-delivery stall (headers, then silence) -> run.stalled +
#     run.retrying (the single retry), a second run.stalled, then
#     run.failed with the stall reason
#
# Usage: harness/tests/stall_watchdog_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MID_MOCK_PORT=8977
MID_DAEMON_PORT=8978
SILENT_MOCK_PORT=8979
SILENT_DAEMON_PORT=8980
MID_BASE="http://127.0.0.1:$MID_DAEMON_PORT/api/v1"
SILENT_BASE="http://127.0.0.1:$SILENT_DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemons run from temp CWDs below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

# Each daemon gets its own CWD so the two nyaxus.db files never contend.
WORKDIR_A="$(mktemp -d)"
WORKDIR_B="$(mktemp -d)"
cd "$WORKDIR_A" || exit 1

MODE=midstream MOCK_PORT="$MID_MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_stall_provider.mjs" > mid_mock.log 2>&1 &
MID_MPID=$!
e2e_port_free "$MID_DAEMON_PORT"
"$DAEMON" --port "$MID_DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MID_MOCK_PORT/v1" --model mock-model > mid_daemon.log 2>&1 &
MID_DPID=$!
cd "$WORKDIR_B" || exit 1
MODE=silent MOCK_PORT="$SILENT_MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_stall_provider.mjs" > silent_mock.log 2>&1 &
SILENT_MPID=$!
e2e_port_free "$SILENT_DAEMON_PORT"
"$DAEMON" --port "$SILENT_DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$SILENT_MOCK_PORT/v1" --model mock-model > silent_daemon.log 2>&1 &
SILENT_DPID=$!
trap 'kill "$MID_DPID" "$SILENT_DPID" "$MID_MPID" "$SILENT_MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

wait_health() {
    local base="$1"
    for _ in $(seq 1 50); do
        if curl -sf "$base/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
wait_health "$MID_BASE" || fail "midstream daemon did not come up (see $WORKDIR_A/mid_daemon.log)"
wait_health "$SILENT_BASE" || fail "silent daemon did not come up (see $WORKDIR_B/silent_daemon.log)"
echo "ok"

# Fast watchdog: 3s silence cap (the 2s socket polls trip it in ~4-5s).
curl -sf -X POST "$MID_BASE/settings" -H 'Content-Type: application/json' \
    -d '{"run.stall_timeout_s":3}' > /dev/null || fail "midstream: setting run.stall_timeout_s failed"
curl -sf -X POST "$SILENT_BASE/settings" -H 'Content-Type: application/json' \
    -d '{"run.stall_timeout_s":3}' > /dev/null || fail "silent: setting run.stall_timeout_s failed"

# start_and_wait <base> <label> -> echoes the terminal status
start_and_wait() {
    local base="$1" label="$2"
    local sid run terminal
    sid="$(curl -sf -X POST "$base/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "$label: no session id"
    run="$(curl -sf -X POST "$base/sessions/$sid/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$sid\",\"message\":\"hello stall\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "$label: run did not start"
    terminal=""
    for _ in $(seq 1 150); do
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
    echo "$terminal"
}

count_events() {
    curl -sf "$1/sessions/$2" | grep -o "\"event_type\":\"$3\"" | wc -l | tr -d ' '
}

echo "== midstream stall (want run.stalled + run.failed, partial text kept) =="
TERMINAL="$(start_and_wait "$MID_BASE" midstream)"
[ "$TERMINAL" = "failed" ] || fail "midstream: expected run.failed, got ${TERMINAL:-none}"
SID="$(curl -sf "$MID_BASE/sessions" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p' | head -1)"
curl -sf "$MID_BASE/sessions/$SID" | grep -q '"event_type":"run.stalled"' || fail "midstream: run.stalled event missing"
curl -sf "$MID_BASE/sessions/$SID" | grep -q 'provider stalled' || fail "midstream: run.failed payload does not name the stall"
curl -sf "$MID_BASE/sessions/$SID" | grep -q 'partial' || fail "midstream: partial text not persisted"
grep -q "provider stalled" "$WORKDIR_A/mid_daemon.log" || fail "midstream: daemon log missing the stall warning"
echo "failed with the stall named (partial text persisted)"

echo "== pre-delivery stall (want run.stalled x2 + run.retrying + run.failed) =="
TERMINAL="$(start_and_wait "$SILENT_BASE" silent)"
[ "$TERMINAL" = "failed" ] || fail "silent: expected run.failed, got ${TERMINAL:-none}"
SID2="$(curl -sf "$SILENT_BASE/sessions" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p' | head -1)"
STALLED="$(count_events "$SILENT_BASE" "$SID2" run.stalled)"
RETRYING="$(count_events "$SILENT_BASE" "$SID2" run.retrying)"
[ "$STALLED" = "2" ] || fail "silent: expected 2 run.stalled events (one per attempt), got $STALLED"
[ "$RETRYING" = "1" ] || fail "silent: expected 1 run.retrying event (the single retry), got $RETRYING"
curl -sf "$SILENT_BASE/sessions/$SID2" | grep -q 'no response for 3s' || fail "silent: run.failed payload missing the stall reason"
echo "failed after one retry (stalled twice)"

echo
echo "STALL_WATCHDOG_TEST_PASSED"
