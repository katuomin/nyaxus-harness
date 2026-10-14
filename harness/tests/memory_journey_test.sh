#!/usr/bin/env bash
# memory_journey_test.sh - per-session journey (the narrative document).
#
# Covers:
#   - manual trigger: POST /memory/journey enqueues a durable job; the worker
#     writes the session's journey from its turns (mock historian)
#   - dedupe: re-enqueueing for the same coverage window is a no-op (same
#     job id); a new turn opens a new window (new job id, watermark advances)
#   - [PREV SESSION] story pointer: a new session for the same profile sees
#     the previous session's journey tail in its system prompt
#   - fork inheritance: a summary fork carries the source's journey, and the
#     fork's run injects [COMPACT CONTEXT] + [JOURNEY]
#   - status reports the journey count
#   - session deletion drops the journey (404 after)
#
# Usage: harness/tests/memory_journey_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MAIN_PORT=9025
JOURNEY_PORT=9026
DAEMON_PORT=9027
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

# last assistant message content
answer() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const asst = (s.messages || []).filter((m) => m.role === "assistant");
            process.stdout.write(asst.length ? asst[asst.length - 1].content : "");
        });'
}

# run_and_wait <session-id> <message>
run_and_wait() {
    local sid="$1" msg="$2" run done=0
    run="$(curl -sf -X POST "$BASE/sessions/$sid/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$sid\",\"message\":\"$msg\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start in $sid"
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            done=1
            break
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run did not complete in $sid"
}

# journey_field <session-id> <field>
journey_field() {
    curl -sf "$BASE/memory/journey?session_id=$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const j = JSON.parse(d);
            const v = j[process.argv[1]];
            process.stdout.write(v === undefined || v === null ? "" : String(v));
        });' "$2"
}

MAIN_PID=""
JOURNEY_PID=""
DAEMON_PID=""
cleanup() {
    kill $DAEMON_PID $MAIN_PID $JOURNEY_PID 2>/dev/null
}
trap cleanup EXIT

MOCK_PORT="$MAIN_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > main.log 2>&1 &
MAIN_PID=$!
MOCK_PORT="$JOURNEY_PORT" "$NODE" "$ROOT/harness/tests/mock_journey_provider.mjs" > journey.log 2>&1 &
JOURNEY_PID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_EXTRACT_URL="http://127.0.0.1:$JOURNEY_PORT/v1" \
NYAXUS_MEMORY_EXTRACT_MODEL=mock-journey \
NYAXUS_AUXILIARY_URL="http://127.0.0.1:$JOURNEY_PORT/v1" \
NYAXUS_AUXILIARY_MODEL=mock-journey \
NYAXUS_MEMORY_RETAIN_EVERY_N_TURNS=1000000 \
NYAXUS_MEMORY_SWEEP_IDLE_SECONDS=3600 \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MAIN_PORT/v1" --model mock-model > daemon.log 2>&1 &
DAEMON_PID=$!

echo "== health =="
READY=""
for _ in $(seq 1 40); do
    if curl -sf "$BASE/health" > /dev/null 2>&1; then READY=1; break; fi
    sleep 0.5
done
[ "$READY" = "1" ] || fail "daemon did not become healthy (daemon.log: $WORKDIR/daemon.log)"
echo "ok"

echo "== custom provider becomes ready (lazy model load) =="
READY=""
for _ in $(seq 1 60); do
    if curl -sf "$BASE/memory/status" 2>/dev/null | grep -q '"ready":true'; then READY=1; break; fi
    sleep 0.5
done
[ "$READY" = "1" ] || fail "custom memory provider did not become ready"
echo "ready"

echo "== profile + first session =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles" \
    -H 'Content-Type: application/json' \
    -d '{"id":"nyx","name":"nyx","description":"journey test profile","identity_prompt":"You are Nyx, a test character.","base_model":"mock-model","temperature":0.2}')"
[ "$CODE" = "200" ] || fail "profile creation returned $CODE"
SID_A="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"nyx"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_A" ] || fail "no session id A"
run_and_wait "$SID_A" "The aurora relay was established at dawn."
run_and_wait "$SID_A" "We agreed to keep the relay secret from the guild."
echo "session A seeded (2 turns)"

echo "== manual journey trigger writes the session's story =="
JOB1="$(curl -sf -X POST "$BASE/memory/journey" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_A\"}" | sed -n 's/.*"job_id":\([0-9]*\).*/\1/p')"
[ -n "$JOB1" ] || fail "journey job was not enqueued"
WRITTEN=""
for _ in $(seq 1 60); do
    if [ -n "$(journey_field "$SID_A" text 2>/dev/null)" ]; then WRITTEN=1; break; fi
    sleep 0.5
done
[ "$WRITTEN" = "1" ] || fail "journey was not written (daemon.log: $WORKDIR/daemon.log)"
TEXT="$(journey_field "$SID_A" text)"
case "$TEXT" in
    *"aurora relay was established at dawn."*) ;;
    *) fail "journey does not cover the seeded turns: $TEXT" ;;
esac
case "$TEXT" in
    *"UNSTAMPED-TURN"*) fail "writer did not stamp turns with real times: $TEXT" ;;
esac
echo "journey written from the turns (stamped)"

echo "== re-enqueue for the same window is a no-op (same job id) =="
JOB1B="$(curl -sf -X POST "$BASE/memory/journey" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_A\"}" | sed -n 's/.*"job_id":\([0-9]*\).*/\1/p')"
[ "$JOB1B" = "$JOB1" ] || fail "same-window re-enqueue returned job $JOB1B, want $JOB1"
echo "deduped (same job id)"

echo "== a new turn opens a new window (new job, watermark advances) =="
WM1="$(journey_field "$SID_A" watermark)"
run_and_wait "$SID_A" "The guild sent an envoy to question us about the relay."
JOB2="$(curl -sf -X POST "$BASE/memory/journey" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_A\"}" | sed -n 's/.*"job_id":\([0-9]*\).*/\1/p')"
[ -n "$JOB2" ] || fail "journey job 2 was not enqueued"
[ "$JOB2" != "$JOB1" ] || fail "new window should enqueue a new job"
WRITTEN=""
for _ in $(seq 1 60); do
    WM2="$(journey_field "$SID_A" watermark 2>/dev/null)"
    if [ -n "$WM2" ] && [ "$WM2" != "$WM1" ]; then WRITTEN=1; break; fi
    sleep 0.5
done
[ "$WRITTEN" = "1" ] || fail "journey watermark did not advance (was $WM1)"
TEXT2="$(journey_field "$SID_A" text)"
case "$TEXT2" in
    *"guild sent an envoy"*) ;;
    *) fail "journey did not gain the new segment: $TEXT2" ;;
esac
echo "watermark advanced, new segment appended"

echo "== [PREV SESSION] carries the story pointer in a new session =="
SID_B="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"nyx"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_B" ] || fail "no session id B"
run_and_wait "$SID_B" "hello"
ANSWER_B="$(answer "$SID_B")"
case "$ANSWER_B" in
    *"[PREV SESSION]"*) ;;
    *) fail "no [PREV SESSION] block in session B: $ANSWER_B" ;;
esac
case "$ANSWER_B" in
    *"story:"*journey-segment*) ;;
    *) fail "[PREV SESSION] has no story pointer: $ANSWER_B" ;;
esac
echo "story pointer delivered"

echo "== summary fork inherits the journey and injects [JOURNEY] =="
SID_C="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d "{\"profile\":\"nyx\",\"fork_from\":\"$SID_A\",\"path\":\"active\",\"mode\":\"summary\"}" \
    | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_C" ] || fail "no fork session id"
TEXT_C="$(journey_field "$SID_C" text 2>/dev/null)"
case "$TEXT_C" in
    *"aurora relay"*) ;;
    *) fail "fork did not inherit the journey: $TEXT_C" ;;
esac
# The fork's initial summary is prepared asynchronously by the mock auxiliary.
SUMREADY=""
for _ in $(seq 1 60); do
    if curl -sf "$BASE/sessions/$SID_C" 2>/dev/null | grep -q '"context_status":"ready"'; then
        SUMREADY=1
        break
    fi
    sleep 0.5
done
[ "$SUMREADY" = "1" ] || fail "fork summary did not become ready"
run_and_wait "$SID_C" "where were we?"
ANSWER_C="$(answer "$SID_C")"
case "$ANSWER_C" in
    *"[COMPACT CONTEXT]"*) ;;
    *) fail "no [COMPACT CONTEXT] in the fork: $ANSWER_C" ;;
esac
case "$ANSWER_C" in
    *"[JOURNEY]"*"journey-segment"*) ;;
    *) fail "no [JOURNEY] block in the fork: $ANSWER_C" ;;
esac
echo "fork carries [COMPACT CONTEXT] + [JOURNEY]"

echo "== status reports the journey count =="
COUNT="$(curl -sf "$BASE/memory/status" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        process.stdout.write(String((JSON.parse(d).custom || {}).journeys ?? 0));
    });')"
[ "$COUNT" -ge 2 ] 2>/dev/null || fail "status journeys count is $COUNT, want >= 2"
echo "status ok ($COUNT journeys)"

echo "== deleting a session drops its journey =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_A/delete" -d '{}')"
[ "$CODE" = "200" ] || fail "session delete returned $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/memory/journey?session_id=$SID_A")"
[ "$CODE" = "404" ] || fail "journey for a deleted session returned $CODE, want 404"
echo "journey dropped with the session"

echo ""
echo "MEMORY_JOURNEY_TEST_PASSED"
