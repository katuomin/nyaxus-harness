#!/usr/bin/env bash
# queue_revival_test.sh - restart queue revival (pre-work for the client truth model).
#
# A prompt queued while an earlier run is streaming is durable at restart:
# the daemon persists its user row (inactive) and the queued run's
# run.accepted. But the queued run never started (the slot was held by the
# run that died), so without revival its row sits inactive forever and the
# operator re-typing it forks a sibling branch. The startup orphan closure
# must revive it: re-anchor the row onto the post-restart leaf (the
# salvaged partial) and start it against the freed slot.
#
#   - run 1 streams a fragment; while it streams, run 2 is enqueued (accepted,
#     queued behind the slot, never started)
#   - the daemon is SIGKILLed mid-stream (run 1 in flight, run 2 queued)
#   - relaunch: run 1 salvages + closes; run 2 is REVIVED - it starts,
#     completes, and its user row re-anchors onto run 1's salvaged partial
#     (a single active tip, no sibling fork)
#
# Usage: harness/tests/queue_revival_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9091
DAEMON_PORT=9092
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
run_json() { "$NODE" -e "$1" "$2" "$3" < <(curl -sf "$4") || fail "$5"; }

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

echo "== wait for run 1 to stream, then enqueue run 2 behind it =="
for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"text.delta"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"text.delta"' \
    || fail "run 1 never streamed a fragment"

RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"second queued prompt\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not enqueue"
[ "$RUN2" != "$RUN1" ] || fail "run 2 got the same id as run 1"

echo "== run 2 must be accepted but NOT started (waiting on the slot) =="
for _ in $(seq 1 50); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q "\"$RUN2\"" && \
       ! curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -A1 "\"$RUN2\"" | grep -q '"run.started"'; then
        break
    fi
    sleep 0.2
done
# run 2 must have a run.accepted/run.queued event in the ledger
curl -sf "$BASE/sessions/$SID" | grep -q "\"$RUN2\"" \
    || fail "run 2 left no ledger event (was not accepted)"
# ...and must NOT have started (run 1 still holds the slot)
if curl -sf "$BASE/sessions/$SID" | node -e '
    let d=""; process.stdin.on("data",c=>d+=c); process.stdin.on("end",()=>{
      const ev=(JSON.parse(d).events||[]);
      const r2=ev.filter(e=>String(e.run_id||"").endsWith(process.argv[1].slice(-12))
                            && e.event_type==="run.started");
      if (r2.length) { console.error("run 2 started before the kill"); process.exit(1); }
      console.log("run 2 accepted, not started (queued behind the slot)");
    });' "$RUN2"; then :; else fail "run 2 started before the kill"; fi

echo "== SIGKILL the daemon (run 1 in flight, run 2 queued) =="
kill -9 "$DPID" 2>/dev/null
wait "$DPID" 2>/dev/null || true
for _ in $(seq 1 40); do
    if ! netstat -ano 2>/dev/null | grep -q ":$DAEMON_PORT .*LISTEN"; then break; fi
    sleep 0.25
done
if netstat -ano 2>/dev/null | grep -q ":$DAEMON_PORT .*LISTEN"; then
    fail "the daemon port did not release after the kill"
fi
echo "daemon killed mid-stream with a queued run"

echo "== relaunch: run 1 salvages + closes, run 2 revives =="
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

echo "== run 2 revives: it starts and completes =="
for _ in $(seq 1 120); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | node -e '
        let d=""; process.stdin.on("data",c=>d+=c); process.stdin.on("end",()=>{
          const ev=(JSON.parse(d).events||[]);
          const r2=ev.filter(e=>String(e.run_id||"").endsWith(process.argv[1].slice(-12))
                                && e.event_type==="run.completed");
          if (r2.length) process.exit(0);
        });' "$RUN2" 2>/dev/null; then
        break
    fi
    sleep 0.25
done
curl -sf "$BASE/sessions/$SID" | node -e '
    let d=""; process.stdin.on("data",c=>d+=c); process.stdin.on("end",()=>{
      const ev=(JSON.parse(d).events||[]);
      const r2=ev.filter(e=>String(e.run_id||"").endsWith(process.argv[1].slice(-12))
                            && e.event_type==="run.completed");
      if (!r2.length) { console.error("run 2 never completed (was not revived)"); process.exit(1); }
      console.log("run 2 revived and completed");
    });' "$RUN2" || fail "run 2 was not revived"

echo "== single active tip: run 2 re-anchors onto run 1's salvaged partial =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const body = JSON.parse(d);
        const msgs = body.messages || [];
        const byRun = (id, role) => msgs.filter((m) =>
            String(m.run_id || "").endsWith(id.slice(-12)) && m.role === role);
        const salvaged = byRun(process.argv[1], "assistant");   // run 1 partial
        const q2user   = byRun(process.argv[2], "user");        // run 2 prompt
        if (salvaged.length !== 1) {
            console.error("expected one salvaged assistant row for run 1, got " + salvaged.length);
            process.exit(1);
        }
        if (q2user.length !== 1) {
            console.error("expected one user row for run 2, got " + q2user.length);
            process.exit(1);
        }
        if (!q2user[0].is_active) {
            console.error("run 2 user row is not active (revival did not re-activate it)");
            process.exit(1);
        }
        if (q2user[0].parent_id !== salvaged[0].id) {
            console.error("run 2 user row did not re-anchor onto the salvaged partial " +
                          "(parent " + q2user[0].parent_id + " != " + salvaged[0].id +
                          ") - sibling fork");
            process.exit(1);
        }
        console.log("run 2 re-anchored onto the salvaged partial (single tip, no fork)");
    });' "$RUN1" "$RUN2" "$BASE/sessions/$SID" "single-tip re-anchor"

echo "QUEUE_REVIVAL_TEST_PASSED"
