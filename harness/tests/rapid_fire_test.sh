#!/usr/bin/env bash
# End-to-end check for rapid-fire messages against the daemon.
#
# Four user turns are sent back-to-back while the first run is still in
# flight (the mock provider delays its answer). All four user messages land
# on the moving active leaf before any run's slot arrives, which is the
# interleaving that used to make the queued-turn re-anchoring point a user
# message at its own descendant (a parent cycle that hung every chain walk).
# Asserts:
#   - every run is accepted (queued), not rejected
#   - all runs complete
#   - every answer is active; none is orphaned by a later turn
#   - the active path is a single acyclic chain covering all eight messages
#   - each run's context ends on its own user message and carries no
#     impossible counts (queued turns land as siblings under the frozen
#     leaf, so a run is not guaranteed to see every earlier queued turn,
#     only that its own message is the last user message it saw)
#
# Usage: harness/tests/rapid_fire_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8921
DAEMON_PORT=8922
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

MOCK_DELAY_MS=2500 MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_slow_provider.mjs" > mock.log 2>&1 &
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

echo "== rapid fire: four messages land while the first run is in flight =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUNS=""
for i in 1 2 3 4; do
    RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"message $i\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$RUN" ] || fail "run $i was rejected while an earlier run was in flight"
    RUNS="$RUNS $RUN"
done
echo "all four runs accepted"

DONE=0
for _ in $(seq 1 300); do
    SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null || true)"
    COMPLETED="$(printf '%s' "$SNAP" | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')"
    if [ "$COMPLETED" -ge 4 ]; then
        DONE=1
        break
    fi
    if printf '%s' "$SNAP" | grep -q '"event_type":"run.failed"'; then
        fail "a run failed: $(printf '%s' "$SNAP" | grep -o '"event_type":"run.failed"[^}]*')"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "all four runs did not complete in time"
echo "all four runs completed"

echo "== assert the active path is one acyclic chain of all four turns =="
curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const runs = process.argv.slice(1).filter(Boolean);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const by = (run, role) => snap.messages.find((m) => m.run_id === run && m.role === role);
    const answers = [];
    for (let i = 0; i < 4; i++) {
      const u = by(runs[i], "user");
      const a = by(runs[i], "assistant");
      if (!u || !a) fail("turn " + (i + 1) + " incomplete");
      if (!u.is_active || !a.is_active) fail("turn " + (i + 1) + " is not fully active");
      // Its own user message was the last user message in the context, and
      // the counts stay within what is possible (at most the other three
      // runs could have finished first; queued turns are siblings under
      // the frozen leaf, so no lower bound beyond its own message).
      const m = /users: (\d+), assistants: (\d+), last: (message \d+)/.exec(a.content);
      if (!m) fail("answer " + (i + 1) + " has unexpected content: " + a.content);
      if (Number(m[2]) > 3) fail("answer " + (i + 1) + " saw " + m[2] + " assistants, want at most 3: " + a.content);
      if (Number(m[1]) < 1) fail("answer " + (i + 1) + " saw no user messages: " + a.content);
      if (m[3] !== "message " + (i + 1)) fail("answer " + (i + 1) + " context did not end on its own message: " + a.content);
      answers.push(a);
    }
    // Walk the parent chain from the active leaf (the active message no
    // other active message parents onto; slot order is not queue order, so
    // the leaf is not necessarily the last queued answer). It must
    // reach the root (parent 0) without visiting any message twice. A
    // re-anchoring cycle (user message parented onto its own descendant)
    // repeats a message; a split tree shows up as multiple tips.
    const byId = new Map(snap.messages.map((m) => [m.id, m]));
    const active = snap.messages.filter((m) => m.is_active);
    const parents = new Set(active.map((m) => m.parent_id));
    const tips = active.filter((m) => !parents.has(m.id));
    if (tips.length !== 1) fail("active tree has " + tips.length + " tips, want exactly 1");
    const chain = [];
    let current = tips[0].id;
    while (current > 0) {
      if (chain.includes(current)) fail("parent chain cycles at message " + current);
      const msg = byId.get(current);
      if (!msg) fail("chain walks into missing message " + current);
      chain.push(current);
      current = msg.parent_id;
    }
    if (chain.length !== 8) fail("active chain has " + chain.length + " messages, want 8");
    console.log("four turns chained acyclically; every context ended on its own message");
  });
' $RUNS || fail "active-path assertions failed"

echo
echo "RAPID_FIRE_TEST_PASSED"
