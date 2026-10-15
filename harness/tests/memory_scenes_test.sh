#!/usr/bin/env bash
# memory_scenes_test.sh - L2 scene blocks + L3 persona (derived distillation).
#
# Covers (layered distillation: L1 units -> L2 scenes -> L3 persona):
#   - the manual consolidate trigger runs the consolidation sweep AND the
#     scene refresh: with 6 seeded units (two topics) and no scenes yet
#     (whole-bank watermark), the (mock) scene model's create decisions are
#     applied to the memory_scenes table
#   - the persona is rewritten from the scenes by the (mock) persona model
#     and lands in the single-row memory_persona table
#   - both generations are logged (kind=scene / kind=persona, prompt id +
#     sha256)
#   - the persona is injected into the stable-tier [CORE MEMORY] block of
#     the next run's prompt (asserted through the main mock's system-echo)
#
# Usage: harness/tests/memory_scenes_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MAIN_PORT=9065
SCENES_PORT=9066
DAEMON_PORT=9067
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
EMBED_MODEL="$ROOT/harness/models/nyx/qwen3-embedding-0.6b-int8.nyx"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$EMBED_MODEL" ] || { echo "embedding model not found: $EMBED_MODEL" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$MAIN_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > main.log 2>&1 &
MAINSID=$!
MOCK_PORT="$SCENES_PORT" "$NODE" "$ROOT/harness/tests/mock_memory_scenes_provider.mjs" > scenes.log 2>&1 &
SCENESID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_EXTRACT_URL="http://127.0.0.1:$SCENES_PORT/v1" \
NYAXUS_MEMORY_EXTRACT_MODEL=mock-scenes \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MAIN_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MAINSID" "$SCENESID" 2>/dev/null || true' EXIT
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

echo "== seed six units (two topics x three) via record approval =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
seed() {
    RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
        -d "{\"text\":\"$1\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$SID\"}" \
        | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
    [ -n "$RECORD_ID" ] || fail "seed record was not created: $1"
    CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$RECORD_ID/approve" -d '{}')"
    [ "$CODE" = "200" ] || fail "approve returned $CODE for: $1"
}
seed "The user drinks Ethiopian coffee every morning."
seed "The user grinds the coffee beans with a hand mill."
seed "The user brews the coffee with a gooseneck kettle."
seed "The user develops the nyaxus harness in C++."
seed "The user prefers minimal dependencies in the build."
seed "The user runs the test suite before every commit."

SEEDS=""
for _ in $(seq 1 90); do
    SEEDS="$(curl -sf "$BASE/memory/bank?limit=200&state=all" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const units = (JSON.parse(d).units) || [];
            process.stdout.write(String(units.length));
        });')"
    [ "${SEEDS:-0}" -ge 6 ] 2>/dev/null && break
    sleep 0.5
done
[ "${SEEDS:-0}" -ge 6 ] 2>/dev/null || fail "only $SEEDS of 6 seeded units in the bank"
echo "seeded $SEEDS units"

echo "== manual consolidate: sweep + scene refresh + persona =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/consolidate" -d '{}')"
[ "$CODE" = "200" ] || fail "consolidate returned $CODE"

SCENES=""
for _ in $(seq 1 90); do
    SCENES="$(curl -sf "$BASE/memory/scenes" 2>/dev/null)" || continue
    if printf '%s' "$SCENES" | grep -q '"persona"' && \
       printf '%s' "$SCENES" | grep -q '"count":2'; then
        break
    fi
    sleep 0.5
done
printf '%s' "$SCENES" | grep -q '"count":2' || fail "scene set not at 2 scenes: $SCENES"
echo "scene set present"

echo "== both topics became scenes (mock create decisions applied) =="
printf '%s' "$SCENES" | grep -q '"name":"coffee-ritual"' || fail "coffee-ritual scene missing: $SCENES"
printf '%s' "$SCENES" | grep -q '"name":"c-plus-plus-work"' || fail "c-plus-plus-work scene missing: $SCENES"
echo "scenes present"

echo "== persona generated from the scenes =="
printf '%s' "$SCENES" | grep -q "Ethiopian coffee and develops the nyaxus harness in C++" \
    || fail "persona content missing: $SCENES"
echo "persona present"

echo "== generations logged (kind=scene + kind=persona, prompt identity) =="
GENS="$(curl -sf "$BASE/memory/generations?limit=50")"
printf '%s' "$GENS" | grep -q '"kind":"scene"' || fail "no kind=scene generation: $GENS"
printf '%s' "$GENS" | grep -q '"prompt_id":"memory-scenes"' || fail "scene prompt id missing: $GENS"
printf '%s' "$GENS" | grep -q '"kind":"persona"' || fail "no kind=persona generation: $GENS"
printf '%s' "$GENS" | grep -q '"prompt_id":"memory-persona"' || fail "persona prompt id missing: $GENS"
echo "generations logged"

echo "== persona injected into the stable-tier [CORE MEMORY] block =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"Say hi.\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"
ANSWER=""
for _ in $(seq 1 75); do
    ANSWER="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const msgs = (s.messages || []).filter((m) => m.role === "assistant");
            process.stdout.write(msgs.length ? msgs[msgs.length - 1].content : "");
        });')"
    if printf '%s' "$ANSWER" | grep -q "CORE MEMORY"; then
        break
    fi
    sleep 0.2
done
printf '%s' "$ANSWER" | grep -q "CORE MEMORY" || fail "core memory block not in the prompt: $ANSWER"
printf '%s' "$ANSWER" | grep -q "User persona" || fail "persona section not in the prompt: $ANSWER"
printf '%s' "$ANSWER" | grep -q "Ethiopian coffee" || fail "persona text not in the prompt: $ANSWER"
echo "persona injected"

echo
echo "MEMORY_SCENES_TEST_PASSED"
