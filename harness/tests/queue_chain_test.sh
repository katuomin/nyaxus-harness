#!/usr/bin/env bash
# queue_chain_test.sh - queued prompts chain onto the conversation tail.
#
# Guards the queue-anchoring bug: a queued turn's user message is appended at
# send time, parented on the then-active leaf (the in-flight reply of the run
# holding the slot). When that run completes and moves the active leaf, the
# queued message must stay active - otherwise the next queued send parents off
# the reply itself and the two queued turns become sibling branches (the
# "1/2" tabs in the UI instead of a continuous thread).
#
# Sequence: run1 streams; "second" queues; run1 completes; "third" queues
# behind run2 while it streams. Run3's provider view must contain both queued
# turns in order (users: 3, assistants: 2), proving each queued user message
# chained after the prior reply.
#
# Usage: harness/tests/queue_chain_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8931
DAEMON_PORT=8932
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

MOCK_PORT="$MOCK_PORT" MOCK_DELAY_MS=1500 "$NODE" "$ROOT/harness/tests/mock_slow_provider.mjs" > mock.log 2>&1 &
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

session_json() { curl -sf "$BASE/sessions/$1"; }

start_run() { # start_run <sid> <message>: returns the run id
    curl -sf -X POST "$BASE/sessions/$1/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$1\",\"message\":\"$2\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p'
}

wait_run_done() { # wait_run_done <sid> <run-id>: that run's terminal event
    local done=""
    for _ in $(seq 1 100); do
        if session_json "$1" 2>/dev/null | "$NODE" -e '
            let d = "";
            process.stdin.on("data", (c) => { d += c; });
            process.stdin.on("end", () => {
                const rid = process.argv[1];
                const s = JSON.parse(d);
                const done = (s.events || []).some((e) =>
                    e.run_id === rid &&
                    (e.event_type === "run.completed" ||
                     e.event_type === "run.failed" ||
                     e.event_type === "run.cancelled"));
                process.stdout.write(done ? "1" : "");
            });' "$2" | grep -q 1; then
            done=1
            break
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run $2 did not complete"
}

assistant_replies() { # assistant_replies <sid>: newline-joined assistant contents
    session_json "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const rows = (JSON.parse(d).messages || [])
                .filter((m) => m.role === "assistant")
                .map((m) => m.content);
            process.stdout.write(rows.join("\n"));
        });'
}

new_session() {
    local sid
    sid="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "no session id"
    printf '%s' "$sid"
}

echo "== queue two prompts while earlier runs hold the slot =="
SID="$(new_session)"
R1="$(start_run "$SID" "first")"
[ -n "$R1" ] || fail "run1 did not start"
sleep 0.3
R2="$(start_run "$SID" "second")"
[ -n "$R2" ] || fail "run2 (queued) was not accepted"
wait_run_done "$SID" "$R1"
# Run1 just completed; run2 holds the slot now. Send immediately so the leaf
# the bug deactivates is run2's in-flight reply parent.
R3="$(start_run "$SID" "third")"
[ -n "$R3" ] || fail "run3 (queued) was not accepted"
wait_run_done "$SID" "$R2"
wait_run_done "$SID" "$R3"
echo "all three runs completed"

echo "== the final turn must see the whole chained conversation =="
REPLIES="$(assistant_replies "$SID")"
printf '%s\n' "$REPLIES" | grep -q "users: 3, assistants: 2, last: third" \
    || fail "queued turns did not chain (run3 saw: $(printf '%s\n' "$REPLIES" | tail -1))"
echo "run3 provider view: $(printf '%s\n' "$REPLIES" | tail -1)"

echo "== active path is one continuous chain =="
ACTIVE="$(session_json "$SID" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const rows = (JSON.parse(d).messages || []).filter((m) => m.is_active).map((m) => m.content);
        process.stdout.write(rows.join("|"));
    });')"
case "$ACTIVE" in
    "first|"*"second|"*"third"*) echo "active path: $ACTIVE" ;;
    *) fail "active path is not a single chain: '$ACTIVE'" ;;
esac

echo "QUEUE_CHAIN_TEST_PASSED"
