#!/usr/bin/env bash
# tool_context_budget_test.sh - a large terminal result must leave room for
# the provider prompt instead of turning a byte cap into a context overflow.
#
# Usage: harness/tests/tool_context_budget_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8960
DAEMON_PORT=8961
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
mkdir -p da
REQUEST_LOG="$WORKDIR/requests.jsonl"
# The daemon's terminal runs a unix shell on every platform (git-bash on
# Windows), so the command is bash-compatible: a path-heavy listing that
# exceeds the terminal output cap.
MOCK_COMMAND='yes C:/nyaxus/project/src/file_000000.txt | head -n 100000'

MOCK_PORT="$MOCK_PORT" MOCK_COMMAND="$MOCK_COMMAND" REQUEST_LOG="$REQUEST_LOG" \
    "$NODE" "$ROOT/harness/tests/mock_large_tool_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model \
    --tool-timeout-ms 120000 ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; echo "--- daemon.log ---" >&2; cat da/daemon.log >&2 2>/dev/null || true; exit 1; }

wait_health() {
    for _ in $(seq 1 50); do
        if curl -sf "$BASE/health" > /dev/null 2>&1; then return 0; fi
        sleep 0.2
    done
    return 1
}

wait_pattern() {
    local sid="$1" pattern="$2"
    for _ in $(seq 1 300); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q "$pattern"; then return 0; fi
        sleep 0.2
    done
    return 1
}

pending_approval() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            const req = [...evs].reverse().find((e) => e.event_type === "approval.requested");
            process.stdout.write(req ? String(req.payload.approval_id || "") : "");
        });'
}

echo "== health =="
wait_health || fail "daemon did not come up"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"list the workspace\"}" | \
    sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"

wait_pattern "$SID" '"approval_id":"call_large"' || fail "large terminal call was not offered"
APR="$(pending_approval "$SID")"
[ "$APR" = "call_large" ] || fail "wrong approval id: $APR"
CODE="$(curl -s -o "$WORKDIR/approve.json" -w "%{http_code}" -X POST \
    "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APR\",\"scope\":\"once\"}")"
[ "$CODE" = "200" ] || fail "approve failed (got $CODE)"

wait_pattern "$SID" '"event_type":"run.completed"' || fail "run failed after large tool output"
curl -sf "$BASE/sessions/$SID" | grep -q "large output was bounded" || \
    fail "provider did not receive the bounded tool result"
curl -sf "$BASE/sessions/$SID" | grep -q "output omitted" || \
    fail "tool result did not report the head+tail reduction"

METRICS="$("$NODE" -e '
const fs = require("fs");
const lines = fs.readFileSync(process.argv[1], "utf8").trim().split(/\r?\n/).filter(Boolean);
if (!lines.length) process.exit(1);
const rows = lines.map((line) => JSON.parse(line));
const last = rows[rows.length - 1];
process.stdout.write(`${rows.length} ${last.request_bytes} ${last.tool_bytes}`);
' "$REQUEST_LOG")" || fail "provider did not record requests"
set -- $METRICS
[ "$1" -ge 2 ] || fail "expected the tool round-trip, got $1 request(s)"
[ "$2" -lt 180000 ] || fail "serialized request remained too large: $2 bytes"
[ "$3" -le 65536 ] || fail "tool result exceeded the 64 KiB budget: $3 bytes"
echo "bounded tool result: $3 bytes; serialized request: $2 bytes"

echo
echo "TOOL_CONTEXT_BUDGET_TEST_PASSED"
