#!/usr/bin/env bash
# memory_save_test.sh - memory.save without the per-call approval gate.
#
# Covers:
#   - a run whose model calls memory.save completes without any
#     approve/deny command (the global memory-enabled check is the only
#     gate; the tool is not offered at all while the bank is unreachable)
#   - the saved facts land in the memory bank (direct retain path) with
#     the requested type (world) and the default type (experience)
#
# Usage: harness/tests/memory_save_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9011
DAEMON_PORT=9012
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

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_memory_save_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== run calls memory.save; it must complete with no approval =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"remember that the vault passphrase is iron-88\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1; break
    fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.waiting_approval"'; then
        fail "memory.save still sits behind the approval gate (run is waiting for a decision)"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete without approval (see $WORKDIR/daemon.log)"
echo "run completed ungated"

echo "== the saved facts are in the memory bank with the right types =="
BANK="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
TYPES="$(printf '%s' "$BANK" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const units = (JSON.parse(d).units) || [];
        const world = units.find((u) => (u.text || "").includes("iron-88"));
        const defaultType = units.find((u) => (u.text || "").includes("relay secret"));
        process.stdout.write(
            (world ? world.type : "missing") + "/" +
            (defaultType ? defaultType.type : "missing"));
    });')"
[ "$TYPES" = "world/experience" ] || fail "wrong unit types (want world/experience, got $TYPES): $BANK"
echo "bank contains both facts, typed world + experience"

echo
echo "MEMORY_SAVE_TEST_PASSED"
