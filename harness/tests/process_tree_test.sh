#!/usr/bin/env bash
# process_tree_test.sh - a timed-out tool must take its whole tree with it.
#
# Covers:
#   - a terminal tool whose command tree ends in a long-lived grandchild
#     (cmd /c ping -n 60) hits the tool timeout and reports it
#   - the grandchild is terminated with the tree (job-object kill), so no
#     orphaned ping survives the timeout
#
# Usage: harness/tests/process_tree_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8950
DAEMON_PORT=8951
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

# The long-lived grandchild and the process counter are platform-specific:
# a nested sh/sleep on POSIX, a nested sh/ping on Windows (the daemon's
# terminal runs a unix shell there, and MSYS mangles `cmd /c`'s single-slash
# flag). Must be set before the mock provider starts (it reads MOCK_COMMAND
# from the env).
if [ "$(uname -s 2>/dev/null)" = "Linux" ]; then
    export MOCK_COMMAND='sh -c "sleep 60"'
    proc_count() { pgrep -c -f "sleep 60" 2>/dev/null || true; }
else
    export MOCK_COMMAND='sh -c "ping -n 60 127.0.0.1"'
    proc_count() {
        cmd.exe /c "tasklist /FI \"IMAGENAME eq ping.exe\"" 2>/dev/null | grep -c "ping\.exe" || true
    }
fi

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_tree_tool_provider.mjs" > mock.log 2>&1 &
MPID=$!
# 3s tool cap: the ~60s ping grandchild must hit the timeout, not finish.
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model \
    --tool-timeout-ms 3000 ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

wait_health() {
    for _ in $(seq 1 50); do
        if curl -sf "$BASE/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

wait_pattern() {
    local sid="$1" pattern="$2"
    for _ in $(seq 1 100); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q "$pattern"; then
            return 0
        fi
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
wait_health || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

BEFORE="$(proc_count)"
echo "baseline tree processes: $BEFORE"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"spawn the child\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"

wait_pattern "$SID" '"approval_id":"call_tree"' || fail "no approval.requested for the tree call"
APR="$(pending_approval "$SID")"
[ "$APR" = "call_tree" ] || fail "pending approval id wrong: $APR"
CODE="$(curl -s -o "$WORKDIR/approve.json" -w "%{http_code}" -X POST "$BASE/runs/$RUN/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APR\",\"scope\":\"once\"}")"
[ "$CODE" = "200" ] || fail "approve failed (got $CODE)"

# The tool must hit its 3s timeout (the grandchild wants ~60s).
wait_pattern "$SID" '"event_type":"tool.completed"' || fail "tool never completed"
curl -sf "$BASE/sessions/$SID" | grep -q "timed out" || fail "no timeout note in the tool result"
echo "tool timed out as expected"

# Give the job-object kill a moment to propagate, then count grandchildren.
sleep 2
AFTER="$(proc_count)"
echo "tree processes after kill: $AFTER"
[ "$AFTER" -le "$BEFORE" ] || fail "orphaned grandchild survived the timeout ($BEFORE -> $AFTER)"
echo "tree terminated"

echo
echo "PROCESS_TREE_TEST_PASSED"
