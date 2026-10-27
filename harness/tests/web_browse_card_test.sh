#!/usr/bin/env bash
# web_browse_card_test.sh - the live "working in a browser" card (milestone 1):
# the camofox fetch tier announces the page it opened via an ephemeral
# tool.progress event, and the result path is unchanged.
#
# Covers:
#   - web.fetch via=camofox completes against the mock facade (open tab,
#     snapshot, close tab) with the [src:N] result
#   - a tool.progress event is ledgered with status=browsing, the page
#     title, the url, and the SAME tool_id as the web.fetch tool.started
#   - the progress event lands in the ledger before the tool.completed
#     (the card is live while the tool runs, not after)
#
# Usage: harness/tests/web_browse_card_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon_linux}"
NODE="${NODE:-node}"
CAMOFOX_PORT=9046
PROVIDER_PORT=9047
DAEMON_PORT=9048
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
command -v "$NODE" >/dev/null 2>&1 || { echo "node not found: $NODE" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$CAMOFOX_PORT" "$NODE" "$ROOT/harness/tests/mock_camofox_server.mjs" > camofox.log 2>&1 &
CAMOPID=$!
MOCK_PORT="$PROVIDER_PORT" CAMOFOX_PORT="$CAMOFOX_PORT" \
    "$NODE" "$ROOT/harness/tests/mock_browse_card_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_WEB_CAMOFOX_ENDPOINT="http://127.0.0.1:$CAMOFOX_PORT" \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$CAMOPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "ok"

echo "== run calls web.fetch via=camofox; no approval needed =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"fetch the camofox page\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1; break
    fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.waiting_approval"'; then
        fail "web.fetch sits behind the approval gate (it is read-only)"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "run completed ungated"

SESSION_JSON="$(curl -sf "$BASE/sessions/$SID")"

echo "== the fetch went through the camofox tier and ledgered its source =="
printf '%s' "$SESSION_JSON" | grep -q "Camofox rendered the test page" \
    || fail "the camofox snapshot text did not reach the model"
printf '%s' "$SESSION_JSON" | grep -q '\[src:1\]' \
    || fail "the fetch is missing the [src:N] id"
echo "camofox tier + source id present"

echo "== a tool.progress event carries the browsing card payload =="
PROGRESS_OK="$(printf '%s' "$SESSION_JSON" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const events = JSON.parse(d).events;
        const progress = events.find((e) => e.event_type === "tool.progress");
        if (!progress || !progress.payload) { console.log("missing"); return; }
        const started = events.find((e) => e.event_type === "tool.started"
            && e.payload && e.payload.name === "web.fetch");
        const completed = events.find((e) => e.event_type === "tool.completed"
            && e.payload && e.payload.name === "web.fetch");
        const p = progress.payload;
        const framed = events.filter((e) => e.event_type === "tool.progress"
            && e.payload && typeof e.payload.frame === "string" && e.payload.frame.startsWith("data:image/png;base64,"));
        const checks = [
            p.status === "browsing",
            p.title === "Camofox Test Page",
            typeof p.url === "string" && p.url.includes("127.0.0.1"),
            started && p.tool_id === started.payload.tool_id,
            progress.id < completed.id,
            // The terminal frame is ledgered before the fetch completes
            // (captured pre-DELETE) and carries the same tool_id.
            framed.length === 1,
            framed.length === 1 && framed[0].id < completed.id,
            framed.length === 1 && framed[0].payload.tool_id === p.tool_id,
        ];
        console.log(checks.every(Boolean) ? "ok" : "mismatch");
    });
')"
[ "$PROGRESS_OK" = "ok" ] || fail "tool.progress card payload failed: $PROGRESS_OK (see $WORKDIR/daemon.log, camofox.log)"
echo "browsing card: status/title/url/tool_id/order all correct"

echo "== the terminal page frame is ledgered as a data URL =="
printf '%s' "$SESSION_JSON" | grep -q 'data:image/png;base64,' \
    || fail "the terminal frame is missing from the ledger"
echo "terminal frame ledgered"

echo
echo "WEB_BROWSE_CARD_TEST_PASSED"
