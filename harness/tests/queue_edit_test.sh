#!/usr/bin/env bash
# End-to-end check for withdrawing queued turns (the UI's edit/remove affordance).
#
# A queued run's user message is appended at send time. Cancelling the run
# while it is still queued must delete that message so it does not dangle on
# the active path, and a later queued turn chained on it must be re-parented
# so its branch stays connected.
#
# Asserts:
#   - a queued run cancelled before it starts gets run.cancelled and no
#     run.started, and its user message is gone from the store
#   - cancelling an earlier queued run re-parents the later queued run's
#     message onto the earlier run's parent, and the later run completes
#     with the withdrawn turn absent from its context
#
# Usage: harness/tests/queue_edit_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8931
DAEMON_PORT=8932
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

MOCK_DELAY_MS=4000 MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_slow_provider.mjs" > mock.log 2>&1 &
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

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

send() {
    curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$1\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p'
}

echo "== part 1: cancel a queued run, its user message must be withdrawn =="
RUN1="$(send "first message")"
[ -n "$RUN1" ] || fail "first run did not start"
RUN2="$(send "second message")"
[ -n "$RUN2" ] || fail "second run was rejected while the first was in flight"
curl -sf -X POST "$BASE/runs/$RUN2/commands" -H 'Content-Type: application/json' \
    -d '{"command":"cancel"}' > /dev/null || fail "cancel command failed"
echo "second run cancelled while queued"

wait_terminal() {
    for _ in $(seq 1 150); do
        SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null || true)"
        if printf '%s' "$SNAP" | grep -q '"event_type":"run.completed"' && \
           printf '%s' "$SNAP" | grep -q '"event_type":"run.cancelled"'; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

wait_terminal || fail "runs did not reach terminal states in time"
echo "terminal states reached"

curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const [run1, run2] = process.argv.slice(1);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const by = (run, role) => snap.messages.find((m) => m.run_id === run && m.role === role);
    const events = (run, type) => snap.events.filter((e) => e.run_id === run && e.event_type === type);
    const u1 = by(run1, "user");
    const a1 = by(run1, "assistant");
    if (!u1 || !a1) fail("first turn incomplete");
    if (!a1.is_active) fail("first answer is not active");
    if (by(run2, "user")) fail("cancelled queued run left its user message behind");
    if (events(run2, "run.cancelled").length !== 1) fail("want exactly one run.cancelled for the withdrawn run");
    if (events(run2, "run.started").length !== 0) fail("withdrawn run must not have started");
    console.log("withdrawn turn is gone; the in-flight answer is untouched");
  });
' "$RUN1" "$RUN2" || fail "part 1 assertions failed"
echo "part 1 ok"

echo "== part 2: withdrawing an earlier queued run keeps the later one connected =="
RUN3="$(send "third message")"
[ -n "$RUN3" ] || fail "third run did not start"
RUN4="$(send "fourth message")"
[ -n "$RUN4" ] || fail "fourth run was rejected"
RUN5="$(send "fifth message")"
[ -n "$RUN5" ] || fail "fifth run was rejected"
curl -sf -X POST "$BASE/runs/$RUN4/commands" -H 'Content-Type: application/json' \
    -d '{"command":"cancel"}' > /dev/null || fail "cancel command failed"
echo "fourth run cancelled while queued"

DONE=0
for _ in $(seq 1 150); do
    SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null || true)"
    COMPLETED="$(printf '%s' "$SNAP" | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')"
    CANCELLED="$(printf '%s' "$SNAP" | grep -o '"event_type":"run.cancelled"' | wc -l | tr -d ' ')"
    if [ "$COMPLETED" -ge 3 ] && [ "$CANCELLED" -ge 2 ]; then
        DONE=1
        break
    fi
    if printf '%s' "$SNAP" | grep -q '"event_type":"run.failed"'; then
        fail "a run failed: $(printf '%s' "$SNAP" | grep -o '"event_type":"run.failed"[^}]*')"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "runs did not reach terminal states in time"
echo "terminal states reached"

curl -sf "$BASE/sessions/$SID" | "$NODE" -e '
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const [run3, run4, run5] = process.argv.slice(1);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const by = (run, role) => snap.messages.find((m) => m.run_id === run && m.role === role);
    const events = (run, type) => snap.events.filter((e) => e.run_id === run && e.event_type === type);
    const a3 = by(run3, "assistant");
    const u5 = by(run5, "user");
    const a5 = by(run5, "assistant");
    if (!a3) fail("third answer missing");
    if (!u5 || !a5) fail("fifth turn incomplete");
    if (by(run4, "user")) fail("cancelled queued run left its user message behind");
    if (events(run4, "run.started").length !== 0) fail("withdrawn run must not have started");
    if (!a5.is_active || !u5.is_active) fail("fifth turn is not active");
    if (u5.parent_id !== a3.id) {
      fail("fifth user message parent is " + u5.parent_id + ", want the third answer " + a3.id);
    }
    // The withdrawn turn must be absent from the fifth run context: three user
    // messages (first, third, fifth) and two answers (first, third).
    if (!/users: 3, assistants: 2/.test(a5.content)) fail("fifth answer context wrong: " + a5.content);
    console.log("later queued turn re-anchored after the in-flight answer; withdrawn turn absent");
  });
' "$RUN3" "$RUN4" "$RUN5" || fail "part 2 assertions failed"
echo "part 2 ok"

echo
echo "QUEUE_EDIT_TEST_PASSED"
