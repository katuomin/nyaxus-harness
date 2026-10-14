#!/usr/bin/env bash
# memory_retain_test.sh - Hindsight retain outbox e2e (slice 1).
#
# Covers:
#   - a completed turn enqueues a durable retain job
#   - the worker drains it against the (mock) Hindsight with the right
#     bank, document id, tags, and content
#   - /api/v1/memory/status reports enabled/healthy/jobs
#   - a provider outage leaves the new job pending (retryable) while the
#     turn still completes
#
# Usage: harness/tests/memory_retain_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
HINDSIGHT_PORT=8971
PROVIDER_PORT=8972
DAEMON_PORT=8973
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

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$HINDSIGHT_PORT" "$NODE" "$ROOT/harness/tests/mock_hindsight.mjs" > hindsight.log 2>&1 &
HPID=$!
MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_URL="http://127.0.0.1:$HINDSIGHT_PORT" \
NYAXUS_MEMORY_BANK=testbank \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$HPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== memory status: enabled, healthy, right bank =="
STATUS="$(curl -sf "$BASE/memory/status")"
printf '%s' "$STATUS" | grep -q '"enabled":true' || fail "memory.enabled not true: $STATUS"
printf '%s' "$STATUS" | grep -q '"healthy":true' || fail "memory not healthy: $STATUS"
printf '%s' "$STATUS" | grep -q '"bank":"testbank"' || fail "wrong bank: $STATUS"
echo "status ok"

echo "== create session + complete a turn =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"remember that the capital of France is Paris\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete"
echo "turn completed"

echo "== wait for the worker to drain the outbox =="
DRAINED=""
for _ in $(seq 1 60); do
    STATUS="$(curl -sf "$BASE/memory/status")"
    if printf '%s' "$STATUS" | grep -q '"completed":1'; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "retain job did not complete: $STATUS"
echo "job completed"

echo "== assert what the (mock) Hindsight received =="
RETAINED="$(curl -sf "http://127.0.0.1:$HINDSIGHT_PORT/__retained")"
COUNT="$(printf '%s' "$RETAINED" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        process.stdout.write(String(JSON.parse(d).length));
    });')"
[ "$COUNT" = "1" ] || fail "expected exactly 1 retained item, got $COUNT"
printf '%s' "$RETAINED" | grep -q "nyaxus-turn:$RUN1" || fail "document id missing"
printf '%s' "$RETAINED" | grep -q "nyaxus.session:$SID" || fail "session tag missing"
printf '%s' "$RETAINED" | grep -q "nyaxus.profile:orchestrator" || fail "profile tag missing"
printf '%s' "$RETAINED" | grep -q "capital of France" || fail "user text missing from content"
printf '%s' "$RETAINED" | grep -q '"bank":"testbank"' || fail "retain hit the wrong bank"
echo "retain payload ok"

echo "== outage: point the provider at a dead port (live apply) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"memory.endpoint":"http://127.0.0.1:1"}')"
[ "$CODE" = "200" ] || fail "could not switch memory endpoint: $CODE"
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"second turn\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "second run did not start"

DONE2=""
for _ in $(seq 1 75); do
    N="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')"
    if [ "$N" = "2" ]; then
        DONE2=1
        break
    fi
    sleep 0.2
done
[ "$DONE2" = "1" ] || fail "second run did not complete (outage must not block turns)"
echo "turn still completes during outage"

echo "== the new job stays pending (retryable) =="
PENDING=""
for _ in $(seq 1 30); do
    STATUS="$(curl -sf "$BASE/memory/status")"
    if printf '%s' "$STATUS" | grep -q '"pending":1'; then
        PENDING=1
        break
    fi
    sleep 0.5
done
[ "$PENDING" = "1" ] || fail "expected a pending job during outage: $STATUS"
printf '%s' "$STATUS" | grep -q '"healthy":false' || fail "expected unhealthy provider: $STATUS"
echo "outage ok"

echo
echo "MEMORY_RETAIN_TEST_PASSED"
