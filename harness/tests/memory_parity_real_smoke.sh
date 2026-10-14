#!/usr/bin/env bash
# memory_parity_real_smoke.sh - manual parity check: the SAME verbatim
# self-recall queries are sent to a real Hindsight bank and to the Nyaxus
# custom provider (which holds the imported copy of that bank), and the
# ranked top-5 results are compared (hit@1, hit@5, set overlap).
#
# Not part of the CI suite (it needs a live Hindsight + the real memory.db);
# run by hand from the agent root.
#
# Usage: harness/tests/memory_parity_real_smoke.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
HINDSIGHT="${HINDSIGHT:?set HINDSIGHT to the live hindsight url; the <deployment-lan> instance is retired (manual-run smoke, not in run_all)}"
BANK="${BANK:-nyaxus}"
PROVIDER_PORT=8993
DAEMON_PORT=8994
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$ROOT/memory.db" ] || { echo "memory.db not found in $ROOT" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

fail() { echo "FAIL: $1" >&2; exit 1; }

curl -sf -m 5 "$HINDSIGHT/version" > /dev/null \
    || fail "Hindsight not reachable at $HINDSIGHT"
echo "== Hindsight reachable: $HINDSIGHT (bank: $BANK) =="

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
# The custom provider needs the real bank; copy it (WAL included, or the
# copy is stale) so the test never writes to the live file.
cp "$ROOT/memory.db" memory.db
[ -f "$ROOT/memory.db-wal" ] && cp "$ROOT/memory.db-wal" memory.db-wal
[ -f "$ROOT/memory.db-shm" ] && cp "$ROOT/memory.db-shm" memory.db-shm

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_DB_PATH="$WORKDIR/memory.db" \
NYAXUS_MEMORY_EMBED_MODEL="${EMBED_MODEL:-qwen3-embedding-0.6b-int8.nyx}" \
NYAXUS_MEMORY_RERANK_MODEL="${RERANK_MODEL:-qwen3-reranker-0.6b-int8.nyx}" \
NYAXUS_MEMORY_RETAIN_EVERY_N_TURNS=999999 \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$PROVPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "== daemon up =="

# The embed/rerank models load lazily on first use; wait until the status
# route reports the custom pipeline ready.
READY=""
for _ in $(seq 1 60); do
    if curl -sf "$BASE/memory/status" 2>/dev/null | grep -q '"ready":true'; then
        READY=1
        break
    fi
    sleep 1
done
[ "$READY" = "1" ] || fail "custom memory pipeline never became ready (see $WORKDIR/daemon.log)"
echo "== custom pipeline ready =="

PARITY_DB="$WORKDIR/memory.db" \
PARITY_HINDSIGHT="$HINDSIGHT" \
PARITY_BANK="$BANK" \
PARITY_DAEMON="$BASE" \
"$NODE" "$ROOT/harness/tests/memory_parity_probe.mjs" || fail "recall parity check failed"

echo
echo "MEMORY_PARITY_REAL_SMOKE_PASSED"
