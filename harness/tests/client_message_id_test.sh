#!/usr/bin/env bash
# client_message_id_test.sh - A-7: client-generated user message ids.
#
# The web client generates its optimistic user row's id (a negative number)
# and sends it with the run. The daemon assigns the row its own monotonic
# snowflake id (ORDER BY id / watermarks stay chronological) and anchors the
# client id on it (client_id, unique), so snapshots and SSE replay dedupe by
# the anchor even when the run id mapping is lost (reconnect mid-flight,
# provisional id race). Checks:
#
#   1. the run response carries the daemon's row id back (positive snowflake)
#   2. the session snapshot carries the user row with client_id anchored
#   3. a collision (same client id already taken) keeps the row, drops the
#      anchor (client_id 0)
#   4. cancelling the queued run withdraws its user row (the A-6 restore
#      relies on this)
#
# Usage: harness/tests/client_message_id_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9095
DAEMON_PORT=9096
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
check() { "$NODE" -e "$1" < <(curl -sf "$2") || fail "$3"; }

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

SID="$("$NODE" -e '
const http = require("http");
const body = JSON.stringify({ profile: "orchestrator" });
const req = http.request("http://127.0.0.1:'$DAEMON_PORT'/api/v1/sessions", {
    method: "POST",
    headers: { "content-type": "application/json", "content-length": Buffer.byteLength(body) },
}, (res) => {
    let data = "";
    res.on("data", (c) => (data += c));
    res.on("end", () => {
        try {
            const j = JSON.parse(data);
            console.log(j.id || j.session_id || "");
        } catch { console.log(""); }
    });
});
req.on("error", () => process.exit(1));
req.end(body);
')"
[ -n "$SID" ] || fail "no session id"
echo "== session: $SID =="

# 1. The response carries the daemon's row id (positive snowflake).
RESP1=$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'content-type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"first turn\",\"message_id\":-123456789}")
[ -n "$RESP1" ] || fail "no run 1 response"
echo "$RESP1" | "$NODE" -e '
let data = "";
process.stdin.on("data", (c) => (data += c));
process.stdin.on("end", () => {
    const j = JSON.parse(data);
    if (!(j.message_id > 0)) {
        console.error("expected a positive daemon message_id, got " + j.message_id);
        process.exit(1);
    }
});
' || fail "run 1 response did not carry a daemon message id"

# 2. The snapshot carries the user row with the client id anchored.
check '
let data = "";
process.stdin.on("data", (c) => (data += c));
process.stdin.on("end", () => {
    const j = JSON.parse(data);
    const user = (j.messages || []).find((m) => m.role === "user");
    if (!user || !(user.id > 0) || user.client_id !== -123456789) {
        console.error("user row missing or client_id not anchored: " + JSON.stringify(user));
        process.exit(1);
    }
});
' "$BASE/sessions/$SID" "snapshot did not anchor the client id on the user row"

# 3. Collision: the same client id is already taken. The row is kept, the
#    anchor is dropped (client_id 0). Run 1 holds the slot (the mock stream
#    is open), so run 2 queues.
RESP2=$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'content-type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"second turn\",\"message_id\":-123456789}")
[ -n "$RESP2" ] || fail "no run 2 response"
RUN2=$(echo "$RESP2" | "$NODE" -e '
let data = "";
process.stdin.on("data", (c) => (data += c));
process.stdin.on("end", () => {
    const j = JSON.parse(data);
    if (!(j.message_id > 0)) {
        console.error("expected a positive daemon message_id, got " + j.message_id);
        process.exit(1);
    }
    console.log(j.run_id || "");
});
')
[ -n "$RUN2" ] || fail "no run 2 id"
check '
let data = "";
process.stdin.on("data", (c) => (data += c));
process.stdin.on("end", () => {
    const j = JSON.parse(data);
    const users = (j.messages || []).filter((m) => m.role === "user");
    if (users.length !== 2) {
        console.error("expected both user rows, got " + users.length);
        process.exit(1);
    }
    const collided = users.find((m) => m.content === "second turn");
    if (!collided || collided.client_id !== undefined) {
        console.error("collision row should have dropped the anchor: " + JSON.stringify(collided));
        process.exit(1);
    }
});
' "$BASE/sessions/$SID" "collision did not keep the row with a dropped anchor"

# 4. Withdrawing the queued run deletes its user row.
curl -sf -X POST "$BASE/runs/$RUN2/commands" -H 'content-type: application/json' \
    -d '{"command":"cancel"}' > /dev/null || fail "cancel of the queued run failed"
sleep 1
check '
let data = "";
process.stdin.on("data", (c) => (data += c));
process.stdin.on("end", () => {
    const j = JSON.parse(data);
    const users = (j.messages || []).filter((m) => m.role === "user");
    if (users.length !== 1 || users[0].client_id !== -123456789) {
        console.error("withdraw left the queued user row behind: " + JSON.stringify(users));
        process.exit(1);
    }
});
' "$BASE/sessions/$SID" "queued user row was not withdrawn on cancel"

echo "PASS: client message ids (snowflake store, client_id anchor, collision, withdraw)"
