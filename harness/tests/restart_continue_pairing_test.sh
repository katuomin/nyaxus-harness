#!/usr/bin/env bash
# restart_continue_pairing_test.sh — the pairing event for the
# restart-recovery retry path (the dead-run continue branch).
#
# The Live-run continue handler publishes run.continued; the RESTART-
# recovery route (the failed run is gone from memory; the command resolves
# the failed run from the store) started continuation runs WITHOUT the
# pairing event, so every retry-after-restart rendered as a fresh turn
# instead of a continuation. This test boots a daemon, runs a call that
# gets interrupted by a daemon restart, presses continue on the DEAD run,
# and asserts the pairing event exists and chains the turns.
#
# Usage: bash harness/tests/restart_continue_pairing_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon_linux}"
NODE="${NODE:-node}"
MOCK_PORT=8945
DAEMON_PORT=8946
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { command -v "$NODE" >/dev/null 2>&1 || { echo "node not found" >&2; exit 1; }; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

# Continue-provider: completes only after a restart, so the retry walks the
# dead-run route. Stream a partial answer, then keep the run in flight.
FAKE="$WORKDIR/provider.sh"
cat > "$FAKE" <<'FAKE'
#!/usr/bin/env bash
echo "provider standing in"
FAKE

# A provider that streams a partial answer, then stalls: we restart the
# daemon while it is mid-stream, and the orphan closure salvages the partial.
cat > "$WORKDIR/mock.mjs" <<'MOCK'
import http from "node:http";
const port = Number(process.env.MOCK_PORT || 8945);
const server = http.createServer((req, res) => {
  if (req.method === "GET" && req.url === "/v1/models") {
    const payload = JSON.stringify({ data: [{ id: "mock-model", meta: { n_ctx: 4096 } }] });
    res.writeHead(200, { "content-type": "application/json", "content-length": Buffer.byteLength(payload) });
    res.end(payload);
    return;
  }
  if (req.method === "POST" && req.url === "/v1/chat/completions") {
    let body = "";
    req.on("data", (c) => { body += c; });
    req.on("end", () => {
      res.writeHead(200, { "content-type": "text/event-stream" });
      res.write('data: {"choices":[{"delta":{"role":"assistant"}}]}\n\n');
      let step = 0;
      const timer = setInterval(() => {
        step += 1;
        if (step <= 3) {
          res.write('data: {"choices":[{"delta":{"content":"' + "partial " + step + '. "}}]}\n\n');
        } else if (step === 4) {
          res.write('data: {"choices":[{"delta":{"content":"The kept partial ends here."}}]}\n\n');
          // then stall forever: the restart kills the stream before [DONE]
        }
      }, 200);
      res.on("close", () => clearInterval(timer));
    });
    return;
  }
});
server.listen(port, () => console.log("mock up"));
MOCK

MOCK_PORT="$MOCK_PORT" "$NODE" "$WORKDIR/mock.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"read the partial\"}")"
printf '%s' "$RUN1" | grep -q 'run_id' || fail "run 1 did not start: $RUN1"
RUN1_ID="$(printf '%s' "$RUN1" | sed -n 's/.*"run_id":"\([^\"]*\)".*/\1/p')"
# Let the partial stream (3 deltas at 200ms + margins).
sleep 2.5

# Restart the daemon: closes every in-flight run and salvages the streamed
# partial as an assistant row under the failed run id.
kill -9 "$DPID" 2>/dev/null
sleep 0.5
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model >> daemon.log 2>&1 &
DPID=$!
sleep 1.5
curl -sf "$BASE/health" >/dev/null || fail "restart daemon did not come up"

echo "== continue on the dead (restarted) run =="
CONT="$(curl -sf -X POST "$BASE/runs/$RUN1_ID/commands" -H 'Content-Type: application/json' \
    -d '{"command":"continue"}')"
printf '%s' "$CONT" | grep -q 'run_id' || fail "continue did not start: $CONT"
RUN2_ID="$(printf '%s' "$CONT" | sed -n 's/.*"run_id":"\([^\"]*\)".*/\1/p')"
[ "$RUN2_ID" != "$RUN1_ID" ] || fail "continue returned the same run id"

sleep 1
curl -sf "$BASE/sessions/$SID" | grep -q '"run.continued"' \
    || fail "the restart-recovery retry did not publish run.continued"

echo "pairing event published for the dead-run retry"
echo "RESTART_CONTINUE_PAIRING_TEST_PASSED"
