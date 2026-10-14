#!/usr/bin/env bash
# memory_recall_test.sh - Hindsight recall path e2e (slice 2).
#
# Covers:
#   - a retained turn is recalled through the provider with the right
#     session tag, budget, and strict tag matching
#   - an unscoped recall searches the whole bank (no tag filter)
#   - an empty query is rejected with 400
#   - a provider outage falls back to local lexical search over the
#     session's own messages
#
# Usage: harness/tests/memory_recall_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
HINDSIGHT_PORT=8981
PROVIDER_PORT=8982
DAEMON_PORT=8983
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
NYAXUS_MEMORY_BANK=recallbank \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$HPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== create session + complete a turn =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"remember that the lighthouse keeper password is seagull-42\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
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

echo "== wait for the worker to drain the retain outbox =="
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

echo "== recall through the provider, scoped to the session =="
RECALL="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d "{\"query\":\"lighthouse keeper password\",\"session\":\"$SID\"}")"
printf '%s' "$RECALL" | grep -q '"source":"hindsight"' || fail "expected provider recall: $RECALL"
printf '%s' "$RECALL" | grep -q 'seagull-42' || fail "recalled text missing: $RECALL"
printf '%s' "$RECALL" | grep -q "nyaxus.session:$SID" || fail "session tag missing from result: $RECALL"
printf '%s' "$RECALL" | grep -q "nyaxus-turn:$RUN1" || fail "document id missing: $RECALL"
echo "scoped recall ok"

echo "== assert what the (mock) Hindsight received =="
RECALLS="$(curl -sf "http://127.0.0.1:$HINDSIGHT_PORT/__recalls")"
printf '%s' "$RECALLS" | grep -q '"bank":"recallbank"' || fail "recall hit the wrong bank: $RECALLS"
printf '%s' "$RECALLS" | grep -q '"tags_match":"all_strict"' || fail "tags_match missing: $RECALLS"
printf '%s' "$RECALLS" | grep -q '"budget":"low"' || fail "budget missing: $RECALLS"
printf '%s' "$RECALLS" | grep -q "nyaxus.session:$SID" || fail "session tag not sent: $RECALLS"
echo "recall payload ok"

echo "== unscoped recall searches the whole bank (no tag filter) =="
RECALL2="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d '{"query":"lighthouse keeper password"}')"
printf '%s' "$RECALL2" | grep -q '"source":"hindsight"' || fail "expected provider recall: $RECALL2"
printf '%s' "$RECALL2" | grep -q 'seagull-42' || fail "unscoped recall missed the item: $RECALL2"
RECALLS2="$(curl -sf "http://127.0.0.1:$HINDSIGHT_PORT/__recalls")"
LAST_TAGS="$(printf '%s' "$RECALLS2" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const arr = JSON.parse(d);
        const t = arr[arr.length - 1].body.tags;
        process.stdout.write(t === undefined ? "undefined" : JSON.stringify(t));
    });')"
[ "$LAST_TAGS" = "undefined" ] || fail "unscoped recall must send no tags, got: $LAST_TAGS"
echo "unscoped recall ok"

echo "== empty query is rejected =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/recall" \
    -H 'Content-Type: application/json' -d '{"query":"   "}')"
[ "$CODE" = "400" ] || fail "expected 400 for empty query, got $CODE"
echo "400 ok"

echo "== outage: point the provider at a dead port (live apply) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"memory.endpoint":"http://127.0.0.1:1"}')"
[ "$CODE" = "200" ] || fail "could not switch memory endpoint: $CODE"

echo "== recall falls back to local lexical search =="
RECALL3="$(curl -sf -X POST "$BASE/memory/recall" -H 'Content-Type: application/json' \
    -d "{\"query\":\"lighthouse keeper password\",\"session\":\"$SID\"}")"
printf '%s' "$RECALL3" | grep -q '"source":"local"' || fail "expected local fallback: $RECALL3"
printf '%s' "$RECALL3" | grep -q '"fallback":true' || fail "fallback flag missing: $RECALL3"
printf '%s' "$RECALL3" | grep -q '"type":"message"' || fail "no message hits: $RECALL3"
printf '%s' "$RECALL3" | grep -q 'seagull-42' || fail "local search missed the turn: $RECALL3"
echo "local fallback ok"

echo
echo "MEMORY_RECALL_TEST_PASSED"
