#!/usr/bin/env bash
# End-to-end check for the terminal child-environment contract.
#
# The daemon is started with secret-shaped NYAXUS_* variables in its own
# environment (the deployment story: a systemd EnvironmentFile). The child
# must NOT see the secret-shaped names (NYAXUS_API_KEY, NYAXUS_*TOKEN,
# NYAXUS_PASSWORD, memory bank/DB paths) but MUST see harmless ones and the
# injected NYAXUS_CMD/MSYSTEM plumbing. terminal.child_env re-shares a
# secret-shaped name by explicit opt-in.
#
# Usage: harness/tests/child_env_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon_linux}"
NODE="${NODE:-node}"
MOCK_PORT=8981
DAEMON_PORT=8982
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || command -v "$NODE" >/dev/null 2>&1 || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# The child prints three vars; the command is a printf of inherited values
# (read-only classification - no gate).
TOOLS='[{"name":"terminal","arguments":{"command":"printf \"KEY=[%s] THR=[%s] TOK=[%s] MSYS=[%s]\\n\" \"${NYAXUS_API_KEY-unset}\" \"${NYAXUS_ENGINE_THREADS-unset}\" \"${NYAXUS_WEB_CAMOFOX_TOKEN-unset}\" \"${MSYSTEM-unset}\""}},null,{"name":"terminal","arguments":{"command":"printf \"KEY=[%s] TOK=[%s]\\n\" \"${NYAXUS_API_KEY-unset}\" \"${NYAXUS_WEB_CAMOFOX_TOKEN-unset}\""}}]'

echo "== health =="
MOCK_PORT="$MOCK_PORT" TOOLS="$TOOLS" "$NODE" "$ROOT/harness/tests/mock_tasks_loop.mjs" > mock.log 2>&1 &
MOCK_PID=$!
e2e_port_free "$DAEMON_PORT"
# The daemon startswith secrets in ITS environment (what a systemd unit's
# EnvironmentFile produces); the model-visible child must not inherit them.
NYAXUS_API_KEY=supersecret0 NYAXUS_WEB_CAMOFOX_TOKEN=tokensecret1 \
NYAXUS_ENGINE_THREADS=2 \
    "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 250); do
    curl -sf "$BASE/health" > /dev/null && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

count_events() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o "\"event_type\":\"$1\"" | wc -l | tr -d ' '
}

COMPLETED_SEEN=0
APPROVALS_SEEN=0
run_and_wait() {
    local run baseline_done baseline_appr approval_id approved=0
    run="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"probe env\"}" \
        | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start"
    baseline_done="$COMPLETED_SEEN"
    baseline_appr="$APPROVALS_SEEN"
    for _ in $(seq 1 350); do
        local done_now appr_now
        done_now="$(count_events run.completed)"
        if [ "$done_now" -gt "$baseline_done" ]; then
            COMPLETED_SEEN="$done_now"
            return 0
        fi
        appr_now="$(count_events run.waiting_approval)"
        if [ "$appr_now" -gt "$baseline_appr" ] && [ "$approved" -eq 0 ]; then
            APPROVALS_SEEN="$appr_now"
            approved=1
            approval_id="$(curl -sf "$BASE/sessions/$SID" \
                | grep -o '"approval_id":"[^"]*"' | tail -1 \
                | sed 's/"approval_id":"//;s/"$//')"
            [ -n "$approval_id" ] || fail "no approval id"
            curl -sf -X POST "$BASE/runs/$run/commands" \
                -H 'Content-Type: application/json' \
                -d "{\"command\":\"approve\",\"approval_id\":\"$approval_id\",\"scope\":\"once\"}" > /dev/null
        fi
        sleep 0.2
    done
    fail "run did not complete: $(tail -3 daemon.log)"
}

terminal_result() {  # $1 = which (1-based)
    curl -sf "$BASE/sessions/$SID" | TR_N="$1" "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const events = JSON.parse(d).events.filter(e => e.event_type === "tool.completed" && e.payload && e.payload.name === "terminal");
            const ev = events[Number(process.env.TR_N) - 1];
            process.stdout.write(ev ? ev.payload.result : "");
        });
    '
}

echo "== child sees harmless vars and hides the secrets =="
run_and_wait
R1="$(terminal_result 1)"
case "$R1" in
    "KEY=[unset]"*) ;;
    *) fail "the child saw NYAXUS_API_KEY: $(printf '%s' "$R1" | head -c 120)" ;;
esac
case "$R1" in
    *'TOK=[unset]'*) ;;
    *) fail "the child saw NYAXUS_WEB_CAMOFOX_TOKEN (not secret-shaped denied)" ;;
esac
case "$R1" in
    *'THR=[2]'*) ;;
    *) fail "the child lost NYAXUS_ENGINE_THREADS: $R1" ;;
esac
case "$R1" in
    *'MSYS=[MINGW64]'*) ;;
    *) fail "the child lost the MSYSTEM plumbing: $R1" ;;
esac
echo "secrets hidden, plumbing intact"

echo "== terminal.child_env re-shares one name =="
curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"terminal.child_env":"NYAXUS_WEB_CAMOFOX_TOKEN"}' > /dev/null \
    || fail "terminal.child_env setting rejected"
run_and_wait
R2="$(terminal_result 2)"
case "$R2" in
    *'TOK=[tokensecret1]'*) ;;
    *) fail "the opt-in name was not re-shared: $R2" ;;
esac
case "$R2" in
    "KEY=[unset]"*) ;;
    *) fail "NYAXUS_API_KEY leaked despite no opt-in: $R2" ;;
esac
echo "opt-in re-share works, others stay hidden"

kill "$DAEMON_PID" 2>/dev/null
kill "$MOCK_PID" 2>/dev/null
wait 2>/dev/null
echo
echo "CHILD_ENV_TEST_PASSED"
