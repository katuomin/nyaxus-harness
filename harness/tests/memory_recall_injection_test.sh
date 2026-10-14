#!/usr/bin/env bash
# memory_recall_injection_test.sh - recall wired into the run pipeline (slice 3).
#
# Covers:
#   - with memory.recall_on_turn enabled, a run recalls session-scoped memory
#     and injects it as a clearly-marked historical-evidence system message
#     (verified via the mock provider echoing the first system message)
- the injected block is wrapped with the profile-neutral memory marker
and a stable id / source reference
#   - a provider outage degrades to the local lexical fallback, which is
#     still injected (a turn never blocks on memory)
#
# Usage: harness/tests/memory_recall_injection_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
HINDSIGHT_PORT=8991
PROVIDER_PORT=8992
DAEMON_PORT=8993
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

# last_assistant <session-json> -> the newest assistant message content
last_assistant() {
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const msgs = (s.messages || []).filter((m) => m.role === "assistant");
            process.stdout.write(msgs.length ? (msgs[msgs.length - 1].content || "") : "");
        });'
}

# wait_for_marker <marker> -> echoes the newest assistant message once it
# contains <marker>; returns non-zero on timeout. The run.completed event is
# shared across a session's runs, so we poll on the distinguishing content
# instead.
wait_for_marker() {
    local marker="$1"
    local _
    for _ in $(seq 1 75); do
        local session answer
        session="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null)" || continue
        answer="$(last_assistant "$session")"
        if printf '%s' "$answer" | grep -qF "$marker"; then
            printf '%s' "$answer"
            return 0
        fi
        sleep 0.2
    done
    return 1
}

MOCK_PORT="$HINDSIGHT_PORT" "$NODE" "$ROOT/harness/tests/mock_hindsight.mjs" > hindsight.log 2>&1 &
HPID=$!
MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_URL="http://127.0.0.1:$HINDSIGHT_PORT" \
NYAXUS_MEMORY_BANK=injbank \
NYAXUS_MEMORY_RECALL_ON_TURN=1 \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$HPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== create session + complete a turn (retains a fact) =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"remember that the lighthouse keeper password is seagull-42\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1; break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "first run did not complete"
echo "turn completed"

echo "== wait for the worker to drain the retain outbox =="
DRAINED=""
for _ in $(seq 1 60); do
    STATUS="$(curl -sf "$BASE/memory/status")"
    if printf '%s' "$STATUS" | grep -q '"completed":1'; then
        DRAINED=1; break
    fi
    sleep 0.5
done
[ "$DRAINED" = "1" ] || fail "retain job did not complete: $STATUS"
echo "job completed"

echo "== second run recalls the retained fact and injects it =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what is the lighthouse keeper password?\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "second run did not start"

ANSWER2="$(wait_for_marker 'source=hindsight')" || fail "second run did not inject provider memory"
# The marker is profile-neutral on purpose (f99402ef5: a character name in
# the label reads as an identity claim to small models).
printf '%s' "$ANSWER2" | grep -q "Memory: past evidence" || fail "injected memory marker missing: $ANSWER2"
printf '%s' "$ANSWER2" | grep -q 'seagull-42' || fail "recalled fact missing from injection: $ANSWER2"
echo "provider injection ok"

echo "== assert the (mock) Hindsight recall was session-scoped =="
RECALLS="$(curl -sf "http://127.0.0.1:$HINDSIGHT_PORT/__recalls")"
printf '%s' "$RECALLS" | grep -q "nyaxus.session:$SID" || fail "session tag not sent: $RECALLS"
printf '%s' "$RECALLS" | grep -q '"bank":"injbank"' || fail "wrong bank: $RECALLS"
echo "recall payload ok"

echo "== outage: point the provider at a dead port (live apply) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"memory.endpoint":"http://127.0.0.1:1"}')"
[ "$CODE" = "200" ] || fail "could not switch memory endpoint: $CODE"

echo "== third run falls back to local lexical search and still injects =="
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what is the lighthouse keeper password?\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "third run did not start"

ANSWER3="$(wait_for_marker 'source=local')" || fail "third run did not inject local fallback"
printf '%s' "$ANSWER3" | grep -q "Memory: past evidence" || fail "local fallback marker missing: $ANSWER3"
printf '%s' "$ANSWER3" | grep -q 'seagull-42' || fail "local fallback missed the turn: $ANSWER3"
echo "local fallback injection ok"

echo
echo "MEMORY_RECALL_INJECTION_TEST_PASSED"
