#!/usr/bin/env bash
# microcompact_test.sh - tier-0 microcompact: when an in-run tool batch
# overflows the context budget, the LLM-free pass clears the old tool-result
# bodies (the 5 newest stay intact) before the auxiliary model is spent.
#
# Usage: harness/tests/microcompact_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8970
AUX_PORT=8971
DAEMON_PORT=8972
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
AUX_COUNTS="$WORKDIR/aux_counts.log"

MOCK_PORT="$MOCK_PORT" REQUEST_LOG="$REQUEST_LOG" \
    "$NODE" "$ROOT/harness/tests/mock_microcompact_provider.mjs" > mock.log 2>&1 &
MPID=$!
MOCK_PORT="$AUX_PORT" COUNT_LOG="$AUX_COUNTS" \
    "$NODE" "$ROOT/harness/tests/mock_aux_counting.mjs" > aux.log 2>&1 &
APID=$!
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model \
    --auxiliary "http://127.0.0.1:$AUX_PORT/v1" --auxiliary-model mock-aux \
    --tool-timeout-ms 120000 ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" "$APID" 2>/dev/null || true' EXIT

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

auto_approve() {
    # Approve every pending gate with session scope (one approval covers all
    # seven terminal calls in the batch).
    local sid="$1" apr code
    apr="$(curl -sf "$BASE/sessions/$sid" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const evs = JSON.parse(d).events || [];
            const req = [...evs].reverse().find((e) => e.event_type === "approval.requested");
            process.stdout.write(req ? String(req.payload.approval_id || "") : "");
        });')"
    [ -n "$apr" ] || return 1
    code="$(curl -s -o /dev/null -w "%{http_code}" -X POST \
        "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
        -d "{\"command\":\"approve\",\"approval_id\":\"$apr\",\"scope\":\"session\"}")"
    [ "$code" = "200" ]
}

echo "== health =="
wait_health || fail "daemon did not come up"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"list the workspace seven times\"}" | \
    sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"

# Drive the run: auto-approve gates until it completes.
for _ in $(seq 1 300); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then break; fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.failed"'; then break; fi
    auto_approve "$SID" || sleep 0.2
done

curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"run.completed"' || \
    fail "run did not complete"

# 1. Tier-0 fired and cleared at least one tool result.
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"context.microcompacted"' || \
    fail "no context.microcompacted event in the ledger"

# 2. The auxiliary model was never spent (no full compaction started, no
#    aux request).
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"context.compaction_started"' && \
    fail "full compaction started; tier-0 was not sufficient"
[ -f "$AUX_COUNTS" ] && fail "auxiliary model was called $(wc -l < "$AUX_COUNTS") time(s)"

# 3. The post-tier-0 request carries the marker and the newest results.
#    Deficit-driven: the clear stops as soon as the request fits again, so
#    the intact count is the deficit's business (2-6 here), not a fixed
#    keep target - but never below the emergency floor.
METRICS="$("$NODE" -e '
const fs = require("fs");
const lines = fs.readFileSync(process.argv[1], "utf8").trim().split(/\r?\n/).filter(Boolean);
if (lines.length < 2) process.exit(1);
const rows = lines.map((line) => JSON.parse(line));
const last = rows[rows.length - 1];
process.stdout.write(`${rows.length} ${last.cleared} ${last.intact}`);
' "$REQUEST_LOG")" || fail "provider did not record the second request"
set -- $METRICS
[ "$1" -ge 2 ] || fail "expected two provider requests, got $1"
[ "$2" -ge 1 ] || fail "second request carried no cleared tool results"
[ "$3" -ge 2 ] || fail "expected the newest results intact, got $3"
[ "$3" -le 6 ] || fail "nothing was cleared down to the budget, got $3 intact"
echo "second request: $2 cleared, $3 intact"

echo
echo "MICROCOMPACT_TEST_PASSED"
