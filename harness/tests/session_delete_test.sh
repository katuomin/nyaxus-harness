#!/usr/bin/env bash
# End-to-end check for session deletion against the daemon.
#
# Asserts:
#   - delete a session -> {"ok":true}, then GET returns 404
#   - delete an unknown session -> 404
#   - delete while a run waits at the approval gate -> the run is cancelled
#     and the delete succeeds (no more silent 409)
#   - delete while a run is stuck in a hanging tool command -> the run is
#     cancelled at the tool timeout and the delete succeeds
#   - in both cases the session does not resurrect after the delete
#
# Usage: harness/tests/session_delete_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8899
DAEMON_PORT=8900
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
HANG_MOCK_PORT=8917
HANG_DAEMON_PORT=8918
HANG_BASE="http://127.0.0.1:$HANG_DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemons run from temp CWDs below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

# Each daemon gets its own CWD so the two nyaxus.db files never contend.
WORKDIR="$(mktemp -d)"
HANGDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
cd "$HANGDIR" || exit 1
MOCK_PORT="$HANG_MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_hang_tool_provider.mjs" > hang_mock.log 2>&1 &
HMPID=$!
# 3s tool cap: the mock's ~30s ping must hit the timeout, not finish.
"$DAEMON" --port "$HANG_DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$HANG_MOCK_PORT/v1" --model mock-model \
    --tool-timeout-ms 3000 > hang_daemon.log 2>&1 &
HDPID=$!
cd "$WORKDIR" || exit 1
trap 'kill "$DPID" "$HDPID" "$MPID" "$HMPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== delete a live session =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
echo "$SID"
DEL="$(curl -s -X POST "$BASE/sessions/$SID/delete" -d '{}')"
printf '%s' "$DEL" | grep -q '"ok":true' || fail "delete did not return ok:true (got: $DEL)"
echo "deleted"

echo "== GET the deleted session (want 404) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/sessions/$SID")"
[ "$CODE" = "404" ] || fail "expected 404 after delete, got $CODE"
echo "404"

echo "== delete an unknown session (want 404) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/does-not-exist/delete" -d '{}')"
[ "$CODE" = "404" ] || fail "expected 404 for unknown session, got $CODE"
echo "404"

echo "== rename a session =="
SIDR="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SIDR" ] || fail "no session id for rename"
REN="$(curl -s -X POST "$BASE/sessions/$SIDR/rename" -H 'Content-Type: application/json' -d '{"title":"renamed thread"}')"
printf '%s' "$REN" | grep -q '"ok":true' || fail "rename did not return ok:true (got: $REN)"
TITLE="$(curl -sf "$BASE/sessions/$SIDR" | sed -n 's/.*"title":"\([^"]*\)".*/\1/p')"
[ "$TITLE" = "renamed thread" ] || fail "title did not update (got: $TITLE)"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SIDR/rename" -H 'Content-Type: application/json' -d '{"title":"   "}')"
[ "$CODE" = "400" ] || fail "expected 400 for blank title, got $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/does-not-exist/rename" -H 'Content-Type: application/json' -d '{"title":"x"}')"
[ "$CODE" = "404" ] || fail "expected 404 for unknown session rename, got $CODE"
echo "rename ok"

echo "== delete while a run waits at the approval gate (want cancel + delete) =="
SID2="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no second session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID2/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID2\",\"message\":\"use the terminal\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"
FOUND=""
for _ in $(seq 1 50); do
    if curl -sf "$BASE/sessions/$SID2" 2>/dev/null | grep -q '"event_type":"approval.requested"'; then
        FOUND=1
        break
    fi
    sleep 0.2
done
[ "$FOUND" = "1" ] || fail "no approval.requested event appeared"
echo "run waiting at approval gate"
DEL2="$(curl -s -X POST "$BASE/sessions/$SID2/delete" -d '{}')"
printf '%s' "$DEL2" | grep -q '"ok":true' || fail "delete of gated session did not return ok:true (got: $DEL2)"
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/sessions/$SID2")"
[ "$CODE" = "404" ] || fail "expected 404 after delete, got $CODE"
# The cancelled run may still be unwinding; give it a moment, then make sure
# it did not resurrect the session's rows on its way out.
sleep 1
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/sessions/$SID2")"
[ "$CODE" = "404" ] || fail "session resurrected after delete, got $CODE"
echo "cancelled + deleted"

echo "== delete while a run is stuck in a hanging tool (want cancel + delete) =="
wait_health() {
    for _ in $(seq 1 50); do
        if curl -sf "$HANG_BASE/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}
wait_health || fail "hang daemon did not come up (see $HANGDIR/hang_daemon.log)"
SID3="$(curl -sf -X POST "$HANG_BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID3" ] || fail "no third session id"
RUN3="$(curl -sf -X POST "$HANG_BASE/sessions/$SID3/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID3\",\"message\":\"use the terminal\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "run did not start"
FOUND=""
for _ in $(seq 1 50); do
    if curl -sf "$HANG_BASE/sessions/$SID3" 2>/dev/null | grep -q '"event_type":"approval.requested"'; then
        FOUND=1
        break
    fi
    sleep 0.2
done
[ "$FOUND" = "1" ] || fail "no approval.requested event appeared"
# Approvals are validated by id: fetch the pending one from the ledger.
APR3="$(curl -sf "$HANG_BASE/sessions/$SID3" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const evs = JSON.parse(d).events || [];
        const req = [...evs].reverse().find((e) => e.event_type === "approval.requested");
        process.stdout.write(req ? String(req.payload.approval_id || "") : "");
    });')"
[ -n "$APR3" ] || fail "no approval id in the ledger"
curl -sf -X POST "$HANG_BASE/runs/$RUN3/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APR3\",\"scope\":\"once\"}" > /dev/null || fail "approve command failed"
FOUND=""
for _ in $(seq 1 50); do
    if curl -sf "$HANG_BASE/sessions/$SID3" 2>/dev/null | grep -q '"event_type":"tool.started"'; then
        FOUND=1
        break
    fi
    sleep 0.2
done
[ "$FOUND" = "1" ] || fail "no tool.started event appeared"
echo "run stuck in the hanging tool"
DEL3="$(curl -s -X POST "$HANG_BASE/sessions/$SID3/delete" -d '{}')"
printf '%s' "$DEL3" | grep -q '"ok":true' || fail "delete of hung-tool session did not return ok:true (got: $DEL3)"
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$HANG_BASE/sessions/$SID3")"
[ "$CODE" = "404" ] || fail "expected 404 after delete, got $CODE"
sleep 1
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$HANG_BASE/sessions/$SID3")"
[ "$CODE" = "404" ] || fail "session resurrected after delete, got $CODE"
echo "cancelled + deleted"

echo
echo "SESSION_DELETE_TEST_PASSED"
