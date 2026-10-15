#!/usr/bin/env bash
# memory_extract_test.sh - aux extraction path with the typed unit taxonomy.
#
# Covers:
#   - a completed turn enqueues a retain job the worker drains through the
#     (mock) extraction endpoint
#   - candidates carry the type taxonomy: grounded world + procedure
#     candidates are stored with their types
#   - an unrecognized type is rejected (invalid)
#   - a candidate not grounded in the turn is rejected (ungrounded)
#
# Usage: harness/tests/memory_extract_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MAIN_PORT=9031
EXTRACT_PORT=9032
DAEMON_PORT=9033
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
EMBED_MODEL="$ROOT/harness/models/nyx/qwen3-embedding-0.6b-int8.nyx"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$EMBED_MODEL" ] || { echo "embedding model not found: $EMBED_MODEL" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$MAIN_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > main.log 2>&1 &
MAINSID=$!
MOCK_PORT="$EXTRACT_PORT" "$NODE" "$ROOT/harness/tests/mock_memory_extract_provider.mjs" > extract.log 2>&1 &
EXTSID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_EXTRACT_URL="http://127.0.0.1:$EXTRACT_PORT/v1" \
NYAXUS_MEMORY_EXTRACT_MODEL=mock-extract \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MAIN_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MAINSID" "$EXTSID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== custom provider becomes ready (lazy model load) =="
READY=""
for _ in $(seq 1 60); do
    STATUS="$(curl -sf "$BASE/memory/status")"
    if printf '%s' "$STATUS" | grep -q '"ready":true'; then
        READY=1
        break
    fi
    sleep 0.5
done
[ "$READY" = "1" ] || fail "custom provider never became ready: $STATUS"
echo "ready"

echo "== one turn; the retain job drains through the (mock) extractor =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"The zephyr protocol listener binds port 4242 on the gateway. Also, run make check to verify the build.\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (see $WORKDIR/daemon.log)"
echo "run completed"

# The bank poll doubles as the drain wait: the startup recovery sweep also
# counts as a completed job, so the status counter alone is ambiguous.
BANK=""
for _ in $(seq 1 90); do
    BANK="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
    if printf '%s' "$BANK" | grep -q "zephyr protocol listener"; then
        break
    fi
    sleep 0.5
done
printf '%s' "$BANK" | grep -q "zephyr protocol listener" || fail "retain job did not store the grounded candidate: $BANK"
STATUS="$(curl -sf "$BASE/memory/status")"
FAILED="$(printf '%s' "$STATUS" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        process.stdout.write(String((JSON.parse(d).jobs || {}).failed || 0));
    });')"
[ "$FAILED" = "0" ] || fail "a memory job failed: $STATUS"
echo "retain job completed"

echo "== grounded candidates stored with their types; invalid + ungrounded rejected =="
BANK="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
RESULT="$(printf '%s' "$BANK" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const units = (JSON.parse(d).units) || [];
        const byText = (needle) => units.find((u) => (u.text || "").includes(needle));
        const world = byText("zephyr protocol listener binds port 4242");
        const procedure = byText("make check");
        const bogus = byText("copper coil");
        const ungrounded = byText("qurix lattice");
        process.stdout.write([
            world ? world.type : "missing",
            procedure ? procedure.type : "missing",
            bogus ? "stored" : "rejected",
            ungrounded ? "stored" : "rejected",
        ].join("/"));
    });')"
[ "$RESULT" = "world/procedure/rejected/rejected" ] || fail "wrong bank state (want world/procedure/rejected/rejected, got $RESULT): $BANK"
echo "world + procedure stored, invalid type + ungrounded candidate rejected"

echo
echo "MEMORY_EXTRACT_TEST_PASSED"
