#!/usr/bin/env bash
# End-to-end check for the native bash-output reduction.
#
# A mock OpenAI-compatible provider answers with two terminal tool calls:
# one emitting 100,000 bytes (must be reduced to head + tail with an exact
# byte-count omission marker while streaming) and one emitting a small
# output (must pass through with no marker). The 64 KiB hard cap is a
# backstop: the reduced result is bounded by the 32 KiB reduction target.
#
# Usage: harness/tests/terminal_reduce_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon_linux}"
NODE="${NODE:-node}"
MOCK_PORT=8971
DAEMON_PORT=8972
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

# JSON field extraction via node (the daemon's objects are nested).
json_field() {
    curl -sf "$1" | PE_EXPR="$2" "$NODE" -e '
        let d = "";
        const expr = new Function("body", "return (" + process.env.PE_EXPR + ")(body);");
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const v = expr(JSON.parse(d));
            process.stdout.write(v === undefined || v === null ? "" : String(v));
        });
    '
}

MOCK_PID=""
DAEMON_PID=""
cleanup() {
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
}
trap cleanup EXIT

# The mock's completion counter is per server lifetime, not per run: the
# null slot ends run 1 (plain text), run 2 gets the small-output call.
# 100,000 bytes (x/newline mix): head 16 KiB + tail 16 KiB survive, the
# dropped middle is 100000 - 32768 = 67232 bytes.
# Tool 4 (capture repair): one CRLF pair collapses to LF, one lone CR is
# kept (console control flow), and an invalid UTF-8 byte (\377 octal,
# printf-portable - \xHH escapes do not exist in dash) becomes U+FFFD;
# the counts surface as an honest capture note in the result.
TOOLS='[{"name":"terminal","arguments":{"command":"yes x | head -c 100000"}},null,{"name":"terminal","arguments":{"command":"echo small-output"}},null,{"name":"terminal","arguments":{"command":"printf \"a\\r\\nb\\377c\""}}]'

echo "== health =="
MOCK_PORT="$MOCK_PORT" TOOLS="$TOOLS" "$NODE" "$ROOT/harness/tests/mock_tasks_loop.mjs" > mock.log 2>&1 &
MOCK_PID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 250); do
    curl -sf "$BASE/health" > /dev/null && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

count_events() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null \
        | grep -o "\"event_type\":\"$1\"" | wc -l | tr -d ' '
}

last_approval_id() {
    curl -sf "$BASE/sessions/$SID" \
        | grep -o '"approval_id":"[^"]*"' | tail -1 \
        | sed 's/"approval_id":"//;s/"$//'
}

APPROVALS_SEEN=0
COMPLETED_SEEN=0

# Start a run and wait for completion. If the terminal call hits the
# approval gate (non-read-only classification), approve it; read-only
# commands (echo) are auto-approved by the classifier and never gate.
# Gate and completion are detected against baselines (the session ledger
# accumulates events across runs, so an absolute count would see the
# previous run's stale gate).
run_and_approve() {  # $1 = message
    local run baseline_appr baseline_done approval_id approved=0
    run="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$1\"}" \
        | sed -n 's/.*"run_id":\"\([^"]*\)\".*/\1/p')"
    [ -n "$run" ] || fail "run did not start"
    baseline_appr="$APPROVALS_SEEN"
    baseline_done="$COMPLETED_SEEN"
    for _ in $(seq 1 350); do
        local appr_now done_now
        appr_now="$(count_events run.waiting_approval)"
        done_now="$(count_events run.completed)"
        if [ "$done_now" -gt "$baseline_done" ]; then
            COMPLETED_SEEN="$done_now"
            return 0
        fi
        # Approve at most once per run: run.waiting_approval is a persistent
        # ledger event, so its count stays above baseline after the first
        # approve and would re-trigger a 409.
        if [ "$appr_now" -gt "$baseline_appr" ] && [ "$approved" -eq 0 ]; then
            APPROVALS_SEEN="$appr_now"
            approved=1
            approval_id="$(last_approval_id)"
            [ -n "$approval_id" ] || fail "no approval id"
            local approve_resp
            approve_resp="$(curl -s -w '|%{http_code}' -X POST "$BASE/runs/$run/commands" \
                -H 'Content-Type: application/json' \
                -d "{\"command\":\"approve\",\"approval_id\":\"$approval_id\",\"scope\":\"once\"}")" \
                || fail "approve command transport error"
            case "$approve_resp" in
                *'|200') ;;
                *) fail "approve command rejected: $approve_resp" ;;
            esac
            continue
        fi
        sleep 0.2
    done
    fail "run did not complete"
}

# The most recent terminal tool result (the test's tool calls are the only
# terminal calls, in order).
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

echo "== run 1: 100 KiB of output is reduced to head + tail =="
run_and_approve "big output"
R1="$(terminal_result 1)"
printf '%s' "$R1" | grep -q "67232 bytes of output omitted" \
    || fail "reduced output lacks the exact omission marker (want '67232 bytes of output omitted')"
R1_LEN="${#R1}"
[ "$R1_LEN" -lt 35000 ] \
    || fail "reduced result is not bounded: $R1_LEN chars (want < 35000)"
[ "$R1_LEN" -gt 30000 ] \
    || fail "reduced result is smaller than expected: $R1_LEN chars (head + tail + marker ~ 32.8K)"
echo "reduced: marker exact, $R1_LEN chars (< 35K, the 64 KiB cap never fires)"

echo "== run 2: small output passes through untouched =="
run_and_approve "small output"
R2="$(terminal_result 2)"
printf '%s' "$R2" | grep -q "small-output" \
    || fail "small output missing: $R2"
printf '%s' "$R2" | grep -q "omitted" \
    && fail "small output got a reduction marker: $R2"
echo "passthrough: no marker"

echo
echo "TERMINAL_REDUCE_TEST_PASSED"

echo "== run 3: capture repair (CRLF + invalid UTF-8) =="
run_and_approve "repair capture"
R3="$(terminal_result 3)"
printf '%s' "$R3" | grep -q "1 CRLF pair(s) normalized to LF" \
    || fail "capture repair note missing the CRLF count: $R3"
printf '%s' "$R3" | grep -q "1 invalid UTF-8 sequence(s) replaced with U+FFFD" \
    || fail "capture repair note missing the UTF-8 count: $R3"
printf '%s' "$R3" | grep -q $'a\nb' \
    || fail "the CRLF pair was not normalized to LF: $R3"
printf '%s' "$R3" | grep -q $'\xef\xbf\xbd' \
    || fail "the invalid byte did not become U+FFFD: $R3"
# The lone CR (console control flow) survives - it is not a broken ending.
printf '%s' "$R3" | grep -q $'b\rc' \
    || fail "the lone CR was dropped or converted: $R3"
echo "capture repair: counts, LF, U+FFFD, and the lone CR all verified"

echo
echo "TERMINAL_REDUCE_TEST_PASSED"
