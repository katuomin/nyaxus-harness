#!/usr/bin/env bash
# memory_feedback_test.sh - offered-memory feedback signal (design principle 5).
#
# Covers:
#   - a recall-on-turn run records an "offered" feedback row for each custom
#     unit it injects (offered is distinct from "used"; recall alone must not
#     raise salience)
#   - the unit detail surfaces the offered count and the last-offered time
#
# Usage: harness/tests/memory_feedback_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9001
DAEMON_PORT=9002
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

# drained_ok <min-completed> -> 1 when at least N jobs completed and none
# pending/sending (background sweep jobs complete alongside retains, so the
# count is a lower bound, not exact).
drained_ok() {
    curl -sf "$BASE/memory/status" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const jobs = (JSON.parse(d).jobs) || {};
            const waiting = Number(jobs.pending || 0) + Number(jobs.sending || 0);
            process.stdout.write(Number(jobs.completed || 0) >= Number(process.argv[1]) && waiting === 0 ? "1" : "0");
        });' "$1"
}

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_RECALL_ON_TURN=1 \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$PROVPID" 2>/dev/null || true' EXIT
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

echo "== seed one approved unit =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
    -d "{\"text\":\"The zephyr protocol listener binds port 4242 on the gateway.\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
[ -n "$RECORD_ID" ] || fail "record was not created"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$RECORD_ID/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE"
DRAINED=""
for _ in $(seq 1 90); do
    if [ "$(drained_ok 1)" = "1" ]; then
        DRAINED=1
        break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "approval retain job did not complete: $(curl -sf "$BASE/memory/status")"
BANK="$(curl -sf "$BASE/memory/bank?limit=50")"
UNIT_ID="$(printf '%s' "$BANK" | sed -n 's/.*"id":"mem:\([0-9]*\)".*/\1/p')"
[ -n "$UNIT_ID" ] || fail "no unit id in bank: $BANK"
echo "unit $UNIT_ID"

echo "== baseline: no offered feedback yet =="
DETAIL="$(curl -sf "$BASE/memory/units/$UNIT_ID")"
OFFERED="$(printf '%s' "$DETAIL" | sed -n 's/.*"offered":\([0-9]*\).*/\1/p')"
[ "$OFFERED" = "0" ] || fail "expected 0 offered before any turn, got '$OFFERED': $DETAIL"
echo "baseline ok"

echo "== a recall-on-turn run offers the unit to the model =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what port does the zephyr protocol use?\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete"
echo "run completed"

echo "== the unit now carries an offered feedback signal =="
DETAIL="$(curl -sf "$BASE/memory/units/$UNIT_ID")"
OFFERED="$(printf '%s' "$DETAIL" | sed -n 's/.*"offered":\([0-9]*\).*/\1/p')"
[ -n "$OFFERED" ] && [ "$OFFERED" -ge 1 ] || fail "expected offered >= 1 after the turn, got '${OFFERED:-0}': $DETAIL"
printf '%s' "$DETAIL" | grep -q '"last_offered_at":"[^"]\{4,\}"' || fail "last_offered_at missing: $DETAIL"
echo "offered=$OFFERED"

echo
echo "MEMORY_FEEDBACK_TEST_PASSED"
