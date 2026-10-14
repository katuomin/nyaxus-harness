#!/usr/bin/env bash
# End-to-end check for the iteration-limit nudge.
#
# The mock provider answers every request with a terminal tool call, so the
# run keeps iterating. When the daemon reaches its final iteration it must
# inject the "maximum number of tool-calling iterations" notice and strip the
# tools; the mock recognizes the notice and returns a text summary instead of
# another tool call. The run then completes with that summary (not a silent
# mid-task stop). With the 20-iteration budget the summary must arrive on
# request 20.
#
# Usage: harness/tests/iteration_limit_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8925
DAEMON_PORT=8926
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_loop_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

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

echo "== health =="
wait_health "$BASE" || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== run keeps tool-calling until the iteration budget is exhausted =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"keep calling tools\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run rejected"

DONE=0
for _ in $(seq 1 600); do
    SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null || true)"
    # Each terminal tool call parks the run at the approval gate; approve it
    # (session scope) so the run keeps iterating to the budget.
    AID="$(printf '%s' "$SNAP" | "$NODE" -e '
      let raw = "";
      process.stdin.on("data", (c) => (raw += c));
      process.stdin.on("end", () => {
        try {
          const snap = JSON.parse(raw);
          const ev = [...(snap.events || [])].reverse().find((e) => e.event_type === "approval.requested" && e.run_id === process.argv[1]);
          console.log(ev ? (ev.payload.approval_id || "") : "");
        } catch { console.log(""); }
      });
    ' "$RUN")"
    if [ -n "$AID" ]; then
        curl -sf -X POST "$BASE/runs/$RUN/commands" -H 'Content-Type: application/json' \
            -d "{\"command\":\"approve\",\"scope\":\"session\",\"approval_id\":\"$AID\"}" > /dev/null
    fi
    if printf '%s' "$SNAP" | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    if printf '%s' "$SNAP" | grep -q '"event_type":"run.failed"' || printf '%s' "$SNAP" | grep -q '"event_type":"run.cancelled"'; then
        fail "run did not complete: $(printf '%s' "$SNAP" | grep -o '"event_type":"run\.\(failed\|cancelled\)"[^}]*')"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete in time"
echo "run completed"

echo "== assert the limit nudge produced a final summary on the last request =="
curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const run = process.argv[1];
    const a = snap.messages.find((m) => m.run_id === run && m.role === "assistant");
    if (!a) fail("no assistant message");
    if (!a.is_active) fail("assistant message not active");
    // The summary names the request count; the 20-iteration budget means the
    // notice (and the tool-less final call) landed on request 20.
    if (!/iteration limit summary after 20 requests/.test(a.content)) {
      fail("final answer is not the limit summary on request 20: " + a.content);
    }
    console.log("run ended with the iteration-limit summary (request 20, tools stripped)");
  });
' "$RUN" || fail "iteration-limit assertions failed"

echo
echo "ITERATION_LIMIT_TEST_PASSED"
