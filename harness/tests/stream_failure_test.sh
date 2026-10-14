#!/usr/bin/env bash
# End-to-end check for provider stream failures against the daemon.
#
# Asserts:
#   - 503 on the first attempt, 200 on the retry -> run.completed with the
#     answer (the "send the message twice to wake the model" case), and a
#     run.retrying event carrying the provider's error detail
#   - persistent 503 -> run.failed whose payload mentions HTTP 503, instead
#     of the old phantom run.completed with 0 tokens
#
# Usage: harness/tests/stream_failure_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
FLAKY_MOCK_PORT=8913
FLAKY_DAEMON_PORT=8914
BROKEN_MOCK_PORT=8915
BROKEN_DAEMON_PORT=8916
FLAKY_BASE="http://127.0.0.1:$FLAKY_DAEMON_PORT/api/v1"
BROKEN_BASE="http://127.0.0.1:$BROKEN_DAEMON_PORT/api/v1"

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

FAIL_TIMES=1 MOCK_PORT="$FLAKY_MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_flaky_provider.mjs" > flaky_mock.log 2>&1 &
MPID=$!
e2e_port_free "$FLAKY_DAEMON_PORT"
"$DAEMON" --port "$FLAKY_DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$FLAKY_MOCK_PORT/v1" --model mock-model > flaky_daemon.log 2>&1 &
DPID=$!
cd "$WORKDIR_B" || exit 1
FAIL_TIMES=999 MOCK_PORT="$BROKEN_MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_flaky_provider.mjs" > broken_mock.log 2>&1 &
BMPID=$!
e2e_port_free "$BROKEN_DAEMON_PORT"
"$DAEMON" --port "$BROKEN_DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$BROKEN_MOCK_PORT/v1" --model mock-model > broken_daemon.log 2>&1 &
BDPID=$!
trap 'kill "$DPID" "$BDPID" "$MPID" "$BMPID" 2>/dev/null || true' EXIT
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
wait_health "$FLAKY_BASE" || fail "flaky daemon did not come up (see $WORKDIR_A/flaky_daemon.log)"
wait_health "$BROKEN_BASE" || fail "broken daemon did not come up (see $WORKDIR_B/broken_daemon.log)"
echo "ok"

echo "== 503 once, then 200 (want run.completed after the retry) =="
SID="$(curl -sf -X POST "$FLAKY_BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$FLAKY_BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"hello flaky\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"
TERMINAL=""
for _ in $(seq 1 100); do
    if curl -sf "$FLAKY_BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        TERMINAL=completed
        break
    fi
    if curl -sf "$FLAKY_BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.failed"'; then
        TERMINAL=failed
        break
    fi
    sleep 0.2
done
[ "$TERMINAL" = "completed" ] || fail "expected run.completed after the retry, got ${TERMINAL:-none}"
curl -sf "$FLAKY_BASE/sessions/$SID" | grep -q 'user: hello flaky' || fail "assistant echo missing from the session"
curl -sf "$FLAKY_BASE/sessions/$SID" | grep -q '"event_type":"run.retrying"' || fail "run.retrying event missing after the 503"
curl -sf "$FLAKY_BASE/sessions/$SID" | grep -q 'server is busy' || fail "run.retrying reason missing the provider error detail"
echo "completed after retry (with run.retrying)"

echo "== persistent 503 (want run.failed with a 503 error) =="
SID2="$(curl -sf -X POST "$BROKEN_BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no second session id"
RUN2="$(curl -sf -X POST "$BROKEN_BASE/sessions/$SID2/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID2\",\"message\":\"hello broken\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run did not start"
TERMINAL=""
for _ in $(seq 1 100); do
    if curl -sf "$BROKEN_BASE/sessions/$SID2" 2>/dev/null | grep -q '"event_type":"run.failed"'; then
        TERMINAL=failed
        break
    fi
    if curl -sf "$BROKEN_BASE/sessions/$SID2" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        TERMINAL=completed
        break
    fi
    sleep 0.2
done
[ "$TERMINAL" = "failed" ] || fail "expected run.failed, got ${TERMINAL:-none}"
curl -sf "$BROKEN_BASE/sessions/$SID2" | grep -q '503' || fail "run.failed payload does not mention HTTP 503"
echo "failed with 503"

echo "== regenerate after failure (want a new run, not a 400) =="
REGEN="$(curl -sf -X POST "$BROKEN_BASE/runs/$RUN2/commands" -H 'Content-Type: application/json' -d '{"command":"regenerate"}')"
echo "$REGEN" | grep -q '"run_id"' || fail "regenerate of a failed run was rejected: $REGEN"
echo "retry accepted (it will 503 again; that is expected)"

echo
echo "STREAM_FAILURE_TEST_PASSED"
