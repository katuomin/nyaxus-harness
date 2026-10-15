#!/usr/bin/env bash
# End-to-end check for the parallel tool wave pool (docs/loop-guards.md
# sibling: the daemon's wave dispatch in execute_run).
#
# The mock provider answers one response with four DISTINCT `sleep 1` terminal
# calls (read-only argv -> one Read wave; distinct so the loop guard does not
# suppress any) and then a text answer. Serial execution takes ~4s wall; the
# wave takes ~1s. Also asserts the wave ledger shape: all four tool.started
# events precede all four tool.completed events, in call order, and each
# completed carries its own call's result.
#
# Usage: harness/tests/wave_probe_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8941
DAEMON_PORT=8942
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_wave_probe_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

for _ in $(seq 1 50); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null 2>&1 || fail "daemon did not come up (see $WORKDIR/daemon.log)"

sid="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$sid" ] || fail "no session id"
run="$(curl -sf -X POST "$BASE/sessions/$sid/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$sid\",\"message\":\"wave probe\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$run" ] || fail "run rejected"

for _ in $(seq 1 600); do
    snap="$(curl -sf "$BASE/sessions/$sid" 2>/dev/null || true)"
    if printf '%s' "$snap" | grep -q '"event_type":"run.completed"'; then break; fi
    if printf '%s' "$snap" | grep -qE '"event_type":"run\.(failed|cancelled)"'; then
        fail "run did not complete: $(printf '%s' "$snap" | grep -o '"event_type":"run\.\(failed\|cancelled\)"[^}]*')"
    fi
    sleep 0.2
done
printf '%s' "$snap" | grep -q '"event_type":"run.completed"' || fail "run did not complete in time"

printf '%s' "$snap" | "$NODE" -e '
  let raw = "";
  process.stdin.on("data", (c) => (raw += c));
  process.stdin.on("end", () => {
    const snap = JSON.parse(raw);
    const fail = (msg) => { console.error("FAIL: " + msg); process.exit(1); };
    const run = process.argv[1];
    const waveIds = ["call-0", "call-1", "call-2", "call-3"];
    const isWave = (e) => waveIds.includes(e.payload.tool_id);
    const ev = (snap.events || []).filter((e) => e.run_id === run);
    const started = ev.filter((e) => e.event_type === "tool.started" && isWave(e));
    const completed = ev.filter((e) => e.event_type === "tool.completed" && isWave(e));
    if (started.length !== 4) fail("expected 4 wave tool.started, got " + started.length);
    if (completed.length !== 4) fail("expected 4 wave tool.completed, got " + completed.length);
    // Wave ledger shape: every started precedes every completed.
    const lastStartedSeq = Math.max(...started.map((e) => e.seq));
    const firstCompletedSeq = Math.min(...completed.map((e) => e.seq));
    if (!(lastStartedSeq < firstCompletedSeq)) {
      fail("started/completed interleaved: max started seq " + lastStartedSeq +
           " >= min completed seq " + firstCompletedSeq + " (serial, not a wave)");
    }
    // Call order in the completed events, and each result carries the
    // output of its own call (no cross-thread result mixup).
    const order = completed.map((e) => e.payload.tool_id);
    if (JSON.stringify(order) !== JSON.stringify(waveIds)) {
      fail("completed order is not call order: " + JSON.stringify(order));
    }
    const letters = "abcd";
    for (let i = 0; i < completed.length; i++) {
      if (!(completed[i].payload.result || "").includes(letters[i])) {
        fail("completed[" + i + "] does not carry the output of call-" + i + ": " +
             JSON.stringify(completed[i].payload.result));
      }
    }
    // Concurrency proof: the runs of the four calls overlap in time. For a
    // wave, the first call is still running when the last one starts
    // (overlap > 0); serial gives overlap < 0. This holds even if a rare
    // MSYS startup hang stretches one call out.
    const tStart = started.map((e) => Date.parse(e.emitted_at));
    const tDone = completed.map((e) => Date.parse(e.emitted_at));
    if (tStart.some(Number.isNaN) || tDone.some(Number.isNaN)) {
      fail("unparseable emitted_at timestamps");
    }
    const overlapMs = Math.min(...tDone) - Math.max(...tStart);
    console.log("overlap " + overlapMs + "ms (serial would be negative)");
    if (overlapMs < 500) {
      fail("calls did not overlap (overlap=" + overlapMs +
           "ms) - the four sleeps ran serially");
    }
    const wallMs = Math.max(...tDone) - Math.min(...tStart);
    console.log("wall " + wallMs + "ms (serial would be ~4000ms)");
    console.log("WAVE_PROBE_PASSED");
  });
' "$run" || fail "probe assertions failed"
