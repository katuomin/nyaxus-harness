#!/usr/bin/env bash
# interrupted_continue_test.sh - retry-that-continues (docs/stream-rules).
#
# A provider stream that closes before [DONE] fails the run and persists
# the partial answer. The ErrorBar's retry must CONTINUE the turn from
# that partial - not regenerate it (which would re-run the tool batch and
# orphan the partial as a dimmed sibling):
#
#   - run 1 fails with "provider closed the stream before [DONE]" while a
#     partial answer is persisted as an assistant message under the run id
#   - POST /runs/<id>/commands {"command":"continue"} starts a run parented
#     on the PARTIAL message (not the user message)
#   - the continuation request carries the partial text as an assistant
#     message and the [CONTINUATION] note as the LAST message (the same
#     system-note-after-trailing-assistant guard the [TURN] note uses
#     against llama.cpp's trailing-assistant echo)
#   - run 2 completes and its committed message chains after the partial,
#     so the transcript reads as one continued turn
#
# Usage: harness/tests/interrupted_continue_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9077
DAEMON_PORT=9078
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

echo "== launch mock provider + daemon =="
MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_continue_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd "$WORKDIR" && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 60); do
    if curl -sf "$BASE/health" > /dev/null 2>&1; then break; fi
    sleep 0.25
done
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "healthy"

SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"orchestrator"}' \
    | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"scan the r9700 batch\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"

for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"run.failed"'; then
        break
    fi
    sleep 0.2
done
SNAP="$(curl -sf "$BASE/sessions/$SID")"
echo "$SNAP" | grep -q "provider closed the stream before \[DONE\]" \
    || fail "run 1 did not fail with the interrupted-stream error"
echo "run 1 failed on the interrupted stream"

echo "== the partial answer persisted under the failed run =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const text = (JSON.parse(d).messages || []).map((m) => String(m.content || ""));
        if (!text.some((t) => t.includes("Continuing with the KV cluster and the"))) {
            console.error("the partial answer was not persisted");
            process.exit(1);
        }
        console.log("partial answer persisted (run 1 row)");
    });' "" "$BASE/sessions/$SID" "partial persistence"

echo "== continue: the new run parents the partial, not the user message =="
RUN2="$(curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"continue\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "continue did not return a run id"
[ "$RUN2" != "$RUN1" ] || fail "continue returned the same run id"

for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"run.completed"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"run.completed"' \
    || fail "run 2 (the continuation) did not complete (see $WORKDIR/daemon.log)"
echo "run 2 completed"

echo "== the continuation request carried the partial + the note =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const reqs = JSON.parse(d);
        if (reqs.length < 2) {
            console.error("expected 2 recorded requests, got " + reqs.length);
            process.exit(1);
        }
        const messages = (reqs[1].messages) || [];
        const last = messages[messages.length - 1];
        if (!last || last.role !== "system" ||
            !String(last.content || "").includes("[CONTINUATION]")) {
            console.error("the continuation request must end on the [CONTINUATION] note");
            process.exit(1);
        }
        const partialAssistant = messages.filter((m) =>
            m.role === "assistant" &&
            String(m.content || "").includes("Continuing with the KV cluster and the"));
        if (partialAssistant.length !== 1) {
            console.error("the continuation request must carry the partial answer as an assistant message once");
            process.exit(1);
        }
        const first = (reqs[0].messages || []);
        if (first.some((m) => String(m.content || "").includes("[CONTINUATION]"))) {
            console.error("the ordinary turn must not carry the [CONTINUATION] note");
            process.exit(1);
        }
        console.log("request shape verified (partial assistant + [CONTINUATION] tail)");
    });' "" "$MOCK/requests" "continuation request shape"

echo "== the transcript reads as one continued turn =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const body = JSON.parse(d);
        const messages = body.messages || [];
        const partial = messages.find((m) => m.role === "assistant" &&
            String(m.content || "").includes("Continuing with the KV cluster and the"));
        const finalRow = messages.find((m) => m.role === "assistant" &&
            String(m.content || "").includes("model card are done"));
        if (!partial || !finalRow) {
            console.error("transcript rows missing (partial or continuation)");
            console.error("roles: " + messages.map((m) => m.role + "<" + String(m.content || "").slice(0, 60) + "> id=" + m.id + " parent=" + m.parent_id).join(" | "));
            process.exit(1);
        }
        if (finalRow.parent_id !== partial.id) {
            console.error("the continuation row must chain after the partial answer");
            process.exit(1);
        }
        console.log("continuation chains after the partial answer");
    });' "" "$BASE/sessions/$SID" "transcript shape"

echo "run 1 stays failed in the ledger; the retry added a continuation, not a regeneration"
echo "INTERRUPTED_CONTINUE_TEST_PASSED"
