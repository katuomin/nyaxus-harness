#!/usr/bin/env bash
# End-to-end check for stream rules: dormant regex rules over
# the streamed assistant text. A rule that fires injects one reminder into
# the prompt-pack volatile tail for the rest of the run - no model
# round-trip, once per rule per run, deduped.
#
# Mock script: run 1 = text "done - all fixed." + a terminal `ls` call in one
# response (the claim precedes any tool execution, so work-claim fires and
# the reminder reaches the next provider call), then an echo of the system
# messages (proves the reminder reached the provider), then "ok". Run 2
# answers "ok" with no claim: the rule must fire exactly once across both
# runs.
#
# Usage: harness/tests/stream_rules_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9023
DAEMON_PORT=9024
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_stream_rules_provider.mjs" > mock.log 2>&1 &
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

# event_count <sid> <type>
event_count() {
    curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            process.stdout.write(String((s.events || []).filter((e) => e.event_type === process.argv[1]).length));
        });' "$2"
}

# nth_assistant <sid> <n> -> the nth (0-based) assistant message content
nth_assistant() {
    curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const msgs = (s.messages || []).filter((m) => m.role === "assistant");
            const m = msgs[Number(process.argv[1])];
            process.stdout.write(m ? (m.content || "") : "");
        });' "$2"
}

wait_run_done() {
    local sid="$1"
    local _
    for _ in $(seq 1 400); do
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

echo "== run 1: claim with no prior tool work, then ls, then the echo =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"fix the build\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"
wait_run_done "$SID" || fail "run 1 did not complete (see $WORKDIR/daemon.log)"
echo "run 1 completed"

echo "== assert: the rule fired once and the reminder reached the provider =="
RULES="$(event_count "$SID" stream.rule)"
echo "stream.rule: $RULES (want 1)"
[ "$RULES" = "1" ] || fail "expected exactly one stream.rule event, got $RULES"
SNAP="$(curl -sf "$BASE/sessions/$SID")"
printf '%s' "$SNAP" | grep -q '"rule":"work-claim"' \
    || fail "the stream.rule event did not name the work-claim rule"
printf '%s' "$SNAP" | grep -q '\[stream rule\] You wrote that the work is done' \
    || fail "the reminder text is not in the ledger"
ECHO_MSG="$(nth_assistant "$SID" 0)"
printf '%s' "$ECHO_MSG" | grep -q '\[stream rule\]' \
    || fail "the reminder never reached the provider: $ECHO_MSG"
echo "reminder reached the provider ok"

echo "== run 2: ok with no claim - the rule must not fire again =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"anything yet?\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not start"
wait_run_done "$SID" || fail "run 2 did not complete (see $WORKDIR/daemon.log)"
RULES_AFTER="$(event_count "$SID" stream.rule)"
echo "stream.rule after run 2: $RULES_AFTER (want 1)"
[ "$RULES_AFTER" = "1" ] || fail "the rule fired again without a new claim; total $RULES_AFTER"

echo "STREAM_RULES_TEST_PASSED"
