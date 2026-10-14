#!/usr/bin/env bash
# orphan_salvage_test.sh - restart salvage (docs/stream-rules).
#
# A run STREAMING text at the moment its daemon dies loses the in-memory
# accumulation - but the narrative is in the ledger (consolidated steps +
# the in-flight step's un-coalesced deltas). The startup orphan closure
# must salvage it into the interrupted turn's partial answer, so the restart
# does not act as a rewind, and the ErrorBar's continue resumes the work:
#
#   - run 1 streams a fragment, then the daemon is SIGKILLed mid-call
#   - relaunch: run 1 closes with "run was interrupted by a daemon restart"
#     AND its streamed text persists as the run's assistant message row
#   - POST continue → run 2 completes; its request reads the salvaged text
#     as an assistant message and ends on the [CONTINUATION] note
#
# Usage: harness/tests/orphan_salvage_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9079
DAEMON_PORT=9080
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
MOCK="http://127.0.0.1:$MOCK_PORT"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }
run_json() { "$NODE" -e "$1" "$2" < <(curl -sf "$3") || fail "$4"; }

e2e_port_free "$DAEMON_PORT"
e2e_port_free "$MOCK_PORT"

start_daemon() {
    ( cd "$WORKDIR" && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) >> daemon.log 2>&1 &
    DPID=$!
    for _ in $(seq 1 60); do
        if curl -sf "$BASE/health" > /dev/null 2>&1; then return 0; fi
        sleep 0.25
    done
    fail "daemon did not come up"
}

echo "== launch mock provider + daemon =="
MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_salvage_provider.mjs" > mock.log 2>&1 &
MPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
start_daemon
echo "healthy"

SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"orchestrator"}' \
    | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"crawl the batch\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"

echo "== wait for the streamed fragment to reach the ledger, then SIGKILL =="
for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"text.delta"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"text.delta"' \
    || fail "the streamed fragment never appeared as text.delta events"
kill -9 "$DPID" 2>/dev/null
wait "$DPID" 2>/dev/null || true
for _ in $(seq 1 40); do
    if ! netstat -ano 2>/dev/null | grep -q ":$DAEMON_PORT .*LISTEN"; then break; fi
    sleep 0.25
done
if netstat -ano 2>/dev/null | grep -q ":$DAEMON_PORT .*LISTEN"; then
    fail "the daemon port did not release after the kill"
fi
echo "daemon killed mid-stream"

echo "== relaunch: the orphan closure salvages the turn =="
start_daemon
for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q 'interrupted by a daemon restart'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q 'interrupted by a daemon restart' \
    || fail "run 1 was not closed as restart-interrupted"
echo "run 1 closed as restart-interrupted"

echo "== the salvaged partial answer persisted under the failed run =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const body = JSON.parse(d);
        const partial = (body.messages || []).filter((m) =>
            m.role === "assistant" &&
            String(m.content || "").includes("Salvage fragment survived the restart"));
        if (partial.length !== 1) {
            console.error("expected exactly one salvaged assistant row, got " + partial.length);
            console.error("roles: " + (body.messages || []).map((m) => m.role + "<" + String(m.content || "").slice(0, 50) + ">").join(" | "));
            process.exit(1);
        }
        if (!partial[0].run_id || partial[0].run_id.slice(-12) !== process.argv[1].slice(-12)) {
            console.error("the salvaged row must carry the failed run id: " + JSON.stringify(partial[0]));
            process.exit(1);
        }
        console.log("salvaged text persisted as the interrupted turn (run 1 row)");
    });' "$RUN1" "$BASE/sessions/$SID" "salvage persistence"

echo "== continue resumes the salvaged work =="
RUN2="$(curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"continue\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "continue did not return a run id"
for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"run.completed"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"run.completed"' \
    || fail "run 2 (the continuation) did not complete"

run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const reqs = JSON.parse(d);
        const messages = ((reqs[1].messages) || []);
        const last = messages[messages.length - 1];
        if (!last || last.role !== "system" ||
            !String(last.content || "").includes("[CONTINUATION]")) {
            console.error("the continuation request must end on the [CONTINUATION] note");
            process.exit(1);
        }
        const salvaged = messages.filter((m) =>
            m.role === "assistant" &&
            String(m.content || "").includes("Salvage fragment survived the restart"));
        if (salvaged.length !== 1) {
            console.error("the continuation must read the salvaged text as assistant context once");
            process.exit(1);
        }
        console.log("continuation continues the salvaged work (request shape verified)");
    });' "" "$MOCK/requests" "continuation request shape"

echo "ORPHAN_SALVAGE_TEST_PASSED"
