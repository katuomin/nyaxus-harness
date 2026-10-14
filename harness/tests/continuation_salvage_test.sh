#!/usr/bin/env bash
# continuation_salvage_test.sh - restart salvage of a CONTINUATION run
# (docs/stream-rules).
#
# A continuation run (continue/retry of a failed turn) has no user row of its
# own. When it is streaming at the moment its daemon dies, the startup orphan
# closure must anchor its salvaged partial onto the CONTINUED turn's user row -
# not onto the current leaf. If a fresh prompt was queued while the
# continuation streamed, that prompt becomes the leaf, and the naive
# "anchor on the leaf" salvage would pull the continuation's partial onto the
# wrong (sibling) branch.
#
#   - run 1 streams a fragment, daemon SIGKILLed mid-call
#   - relaunch: run 1 salvages + closes restart-interrupted
#   - continue run 1 -> run 2 (a continuation, no user row); it streams a
#     fragment
#   - while run 2 streams, send run 3 (a fresh prompt, queued behind run 2)
#   - SIGKILL (run 2 streaming, run 3 queued)
#   - relaunch: run 2's partial salvages onto run 1's user row (NOT run 3's),
#     and run 3 revives + completes
#
# Usage: harness/tests/continuation_salvage_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9097
DAEMON_PORT=9098
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

kill_daemon() {
    kill -9 "$DPID" 2>/dev/null
    wait "$DPID" 2>/dev/null || true
    for _ in $(seq 1 40); do
        if ! netstat -ano 2>/dev/null | grep -q ":$DAEMON_PORT .*LISTEN"; then return 0; fi
        sleep 0.25
    done
    fail "the daemon port did not release after the kill"
}

echo "== launch mock provider + daemon =="
MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_continuation_salvage_provider.mjs" > mock.log 2>&1 &
MPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
start_daemon
echo "healthy"

SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"orchestrator"}' \
    | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"first turn\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"

echo "== run 1 streams, then SIGKILL =="
for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"text.delta"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"text.delta"' \
    || fail "run 1's fragment never appeared as text.delta events"
kill_daemon
echo "daemon killed mid run 1"

echo "== relaunch: run 1 salvages + closes =="
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

echo "== continue run 1 -> run 2 (a continuation) =="
RUN2="$(curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"continue\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "continue did not return a run id"
[ "$RUN2" != "$RUN1" ] || fail "the continuation must be a new run"
for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q 'Continuation fragment survived the restart'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q 'Continuation fragment survived the restart' \
    || fail "run 2's continuation fragment never appeared"
echo "run 2 (continuation) streaming"

echo "== queue run 3 behind the in-flight continuation, then SIGKILL =="
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"queued turn\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "run 3 did not start"
kill_daemon
echo "daemon killed (run 2 streaming, run 3 queued)"

echo "== relaunch: run 2 salvages onto run 1's turn; run 3 revives =="
start_daemon
for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"run.completed"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"run.completed"' \
    || fail "run 3 (the revived queued run) did not complete"
echo "run 3 revived and completed"

echo "== run 2's partial anchored onto run 1's user row, not run 3's =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const body = JSON.parse(d);
        const msgs = body.messages || [];
        const byContent = (needle) => msgs.filter((m) =>
            String(m.content || "").includes(needle));
        const run1User = byContent("first turn").find((m) => m.role === "user");
        const run3User = byContent("queued turn").find((m) => m.role === "user");
        const partial = byContent("Continuation fragment survived the restart");
        if (!run1User) { console.error("run 1 user row missing"); process.exit(1); }
        if (!run3User) { console.error("run 3 user row missing"); process.exit(1); }
        if (partial.length !== 1) {
            console.error("expected exactly one continuation salvaged row, got " + partial.length);
            process.exit(1);
        }
        const parent = partial[0].parent_id;
        if (parent !== run1User.id) {
            console.error("the continuation partial must anchor on run 1 user row "
                + run1User.id + ", got parent " + parent);
            process.exit(1);
        }
        if (parent === run3User.id) {
            console.error("the continuation partial landed on the queued run 3 row "
                + run3User.id + " (the leaf) instead of run 1");
            process.exit(1);
        }
        console.log("continuation partial anchored on the continued turn (run 1 user row)");
        // The prompt queued while the continuation streamed must chain UNDER
        // the salvaged partial, not sit as a sibling of the continued user
        // row - otherwise the transcript forks into a branch.
        if (run3User.parent_id !== partial[0].id) {
            console.error("the queued run 3 prompt must chain under the continuation partial "
                + partial[0].id + ", got parent " + run3User.parent_id);
            process.exit(1);
        }
        console.log("queued prompt chained under the continuation partial");
    });' "" "$BASE/sessions/$SID" "continuation salvage anchoring"

echo "CONTINUATION_SALVAGE_TEST_PASSED"
