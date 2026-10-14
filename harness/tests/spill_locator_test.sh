#!/usr/bin/env bash
# End-to-end check for the spill locator: the
# terminal's dropped middle stays retrievable. Output past the reduce
# target (32 KiB) is persisted verbatim in the session's spill store, and
# the omission marker carries an opaque locator + exact byte count + the
# in-band retrieval guidance. spill.read then reads the stored middle
# byte-exactly.
#
# Mock script (one run, three provider calls):
#   1. terminal "seq 1 20000" (109 KiB) -> reduced; middle spilled
#   2. spill.read (offset 40000, limit 400) using the locator from call 1's
#      tool message (in-band, same run)
#   3. text
#
# Asserts:
#   - the omission marker names the locator and the exact total byte count
#   - one tool.spilled ledger event
#   - the spill file exists in the session's spill store
#   - the spill.read slice is byte-identical to the same byte range of the
#     original output (lossless round-trip)
#
# Usage: harness/tests/spill_locator_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9041
DAEMON_PORT=9042
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

CALLS='[{"name":"terminal","arguments":{"command":"seq 1 20000"}},{"name":"spill.read","arguments":{"id":"$LOCATOR","offset":"40000","limit":"400"}},{"text":"ok"}]'

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

CALLS="$CALLS" MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_spill_provider.mjs" > mock.log 2>&1 &
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

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
echo "$SID"

run_and_wait() {
    local message="$1"
    curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$message\"}" > /dev/null
    local _
    for _ in $(seq 1 400); do
        if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== run 1: seq 1 20000 (109 KiB), then spill.read in the same run =="
run_and_wait "dump the sequence then read its middle" || fail "run did not complete (see $WORKDIR/daemon.log)"
SNAP="$(curl -sf "$BASE/sessions/$SID")"

printf '%s' "$SNAP" | grep -q 'bytes of output omitted - full output (108894 bytes) stored as spill locator \\"spill:' \
    || fail "the omission marker does not name the spill locator and exact total"
printf '%s' "$SNAP" | grep -q 'read the middle in-band with spill.read' \
    || fail "the marker does not carry the in-band retrieval guidance"
SPILLED="$(printf '%s' "$SNAP" | grep -o '"event_type":"tool.spilled"' | wc -l | tr -d ' ')"
echo "tool.spilled: $SPILLED (want 1)"
[ "$SPILLED" = "1" ] || fail "expected one tool.spilled provenance event, got $SPILLED"

echo "== the spill file exists in the session's spill store =="
ls "spills/$SID" > /dev/null 2>&1 || fail "no spills/$SID directory in the workdir"
echo "ok"

echo "== run 2 removed: the in-band read is call 2 of the same run =="
# The spill.read result must carry the slice bounds...
printf '%s' "$SNAP" | grep -q '\[spill spill:[^ ]* bytes 40000..40400 of 108894\]' \
    || fail "the spill.read result header is wrong"
# ...and the slice must be byte-identical to the original output's range.
curl -sf "$BASE/sessions/$SID" -o spill-snap.json
"$NODE" -e '
const fs = require("node:fs");
const s = JSON.parse(fs.readFileSync("./spill-snap.json", "utf8"));
const tools = (s.events || []).filter((e) => e.event_type === "tool.completed");
const read = tools.map((e) => String(e.payload.result || "")).filter((r) => r.startsWith("[spill ")).pop();
if (!read) { console.error("FAIL: no spill.read result"); process.exit(1); }
const slice = read.slice(read.indexOf("]\n") + 2);
// Regenerate the exact byte range of the original output in-process (the
// shell here is cmd.exe: no seq/dd available).
let all = "";
for (let i = 1; i <= 20000; ++i) all += i + "\n";
const expected = all.slice(40000, 40400);
if (slice !== expected) {
  console.error("FAIL: slice is not byte-identical (got " + JSON.stringify(slice.slice(0, 80)) + " ...)");
  process.exit(1);
}
console.log("slice byte-identical to the original range");
' || fail "the spill.read slice did not round-trip byte-exactly"

echo "SPILL_LOCATOR_TEST_PASSED"
