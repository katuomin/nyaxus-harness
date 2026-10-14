#!/usr/bin/env bash
# End-to-end check for steer: a queued message that interjects at the next
# pause of the active run.
#
# The mock provider makes request 1 stream a text fragment plus a terminal
# tool call (a tool iteration), and only request 2 the final answer. The test
# queues a second message while the first run is in its tool iteration and
# steers the first run. The steered run must:
#   - complete (not cancel, not fail) after the tool batch, before request 2
#     (its persisted answer carries only the "working on it" fragment)
#   - hand the slot to the queued run, whose context contains the steered
#     run's partial answer (users: 2, assistants: 1)
#   - leave the active path as one acyclic chain of all four messages
#
# Usage: harness/tests/steer_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8923
DAEMON_PORT=8924
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

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_steer_provider.mjs" > mock.log 2>&1 &
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

echo "== run 1 starts (tool iteration in flight), run 2 queued, run 1 steered =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"message 1\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 rejected"
sleep 0.7
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"message 2\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 was rejected while run 1 was in flight"
STEER_CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RUN1/commands" \
    -H 'Content-Type: application/json' -d '{"command":"steer"}')"
[ "$STEER_CODE" = "200" ] || fail "steer command returned $STEER_CODE, want 200"
echo "steer accepted"

DONE=0
for _ in $(seq 1 300); do
    SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null || true)"
    # The mock's terminal tool call parks run 1 at the approval gate; approve
    # it (session scope) so the run reaches the steer boundary.
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
    ' "$RUN1")"
    if [ -n "$AID" ]; then
        curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
            -d "{\"command\":\"approve\",\"scope\":\"session\",\"approval_id\":\"$AID\"}" > /dev/null
    fi
    COMPLETED="$(printf '%s' "$SNAP" | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')"
    if [ "$COMPLETED" -ge 2 ]; then
        DONE=1
        break
    fi
    if printf '%s' "$SNAP" | grep -q '"event_type":"run.failed"' || printf '%s' "$SNAP" | grep -q '"event_type":"run.cancelled"'; then
        fail "a run did not complete: $(printf '%s' "$SNAP" | grep -o '"event_type":"run\.\(failed\|cancelled\)"[^}]*')"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "both runs did not complete in time"
echo "both runs completed"

echo "== assert the steer ended run 1 at the boundary and run 2 saw the partial answer =="
curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const runs = process.argv.slice(1);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const by = (run, role) => snap.messages.find((m) => m.run_id === run && m.role === role);
    const u1 = by(runs[0], "user"), a1 = by(runs[0], "assistant");
    const u2 = by(runs[1], "user"), a2 = by(runs[1], "assistant");
    if (!u1 || !a1 || !u2 || !a2) fail("turns incomplete");
    if (!u1.is_active || !a1.is_active || !u2.is_active || !a2.is_active) fail("not all messages active");
    // Run 1 must have ended at the boundary: only the pre-tool fragment was
    // persisted, and the final answer (request 2) never happened.
    if (!/working on it/.test(a1.content)) fail("run 1 answer missing the fragment: " + a1.content);
    if (/final:/.test(a1.content)) fail("run 1 ran past the steer boundary: " + a1.content);
    // Run 2 must have seen the full chain including run 1 partial answer.
    if (!/users: 2, assistants: 1, last: message 2/.test(a2.content)) {
      fail("run 2 context wrong: " + a2.content);
    }
    // The active path is one acyclic chain of all four messages.
    const active = snap.messages.filter((m) => m.is_active);
    const parents = new Set(active.map((m) => m.parent_id));
    const tips = active.filter((m) => !parents.has(m.id));
    if (tips.length !== 1) fail("active tree has " + tips.length + " tips, want 1");
    const byId = new Map(snap.messages.map((m) => [m.id, m]));
    const chain = [];
    let current = tips[0].id;
    while (current > 0) {
      if (chain.includes(current)) fail("parent chain cycles at message " + current);
      const msg = byId.get(current);
      if (!msg) fail("chain walks into missing message " + current);
      chain.push(current);
      current = msg.parent_id;
    }
    if (chain.length !== 4) fail("active chain has " + chain.length + " messages, want 4");
    console.log("steer ended run 1 at the boundary; run 2 saw the partial answer");
  });
' "$RUN1" "$RUN2" || fail "steer assertions failed"

echo
echo "STEER_TEST_PASSED"
