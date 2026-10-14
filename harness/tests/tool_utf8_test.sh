#!/usr/bin/env bash
# tool_utf8_test.sh - non-UTF-8 tool output must not break the provider round-trip.
#
# Covers:
#   - a terminal command whose output is not valid UTF-8 (Windows console
#     code-page bytes, e.g. `dir` on cp850) is sanitized before the next
#     provider request, so the provider's JSON parser accepts the round-trip
#     (previously: HTTP 500, json parse_error.101 on the ill-formed byte)
#
# Usage: harness/tests/tool_utf8_test.sh [path-to-nyaxus_daemon]
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
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
# The daemon stores nyaxus.db in its CWD.
mkdir -p da
# Probe file: two bytes that are not valid UTF-8 (cp850-style) between ASCII
# markers, so the tool visibly read the file.
printf 'before\x80\x81after' > da/utf8-probe.txt

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_utf8_tool_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
for _ in $(seq 1 50); do
    if curl -sf "$BASE/health" > /dev/null 2>&1; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"read the probe file\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"

echo "== run prints non-UTF-8 bytes; the round-trip must survive =="
# The terminal call is approval-gated; approve the pending gate.
for _ in $(seq 1 50); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"approval.requested"'; then
        break
    fi
    sleep 0.2
done
APR="$(curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
      const evs = JSON.parse(d).events || [];
      const req = [...evs].reverse().find((e) => e.event_type === "approval.requested");
      process.stdout.write(req ? String(req.payload.approval_id || "") : "");
    });')"
[ -n "$APR" ] || fail "no approval.requested in the ledger"
CODE="$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/runs/$RUN/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APR\",\"scope\":\"once\"}")"
[ "$CODE" = "200" ] || fail "approve failed (got $CODE)"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.failed"'; then
        fail "run failed (provider rejected the round-trip): $(curl -sf "$BASE/sessions/$SID" | grep -o '"error":"[^"]*"' | head -1)"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (see $WORKDIR/daemon.log)"
curl -sf "$BASE/sessions/$SID" | grep -q "utf8 round-trip ok" || fail "final text missing"
echo "non-UTF-8 tool output round-tripped"

echo
echo "TOOL_UTF8_TEST_PASSED"
