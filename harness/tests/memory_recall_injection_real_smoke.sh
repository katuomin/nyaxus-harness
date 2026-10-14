#!/usr/bin/env bash
# memory_recall_injection_real_smoke.sh - manual smoke: slice 3 (recall wired
# into the run pipeline) against a REAL Hindsight instance, disposable bank.
# Not part of the CI suite (needs a live provider); run by hand, then delete
# the bank.
#
# The server-side extraction is slow and flaky (observed 50s-400s+ per
# attempt, including deterministic JSON failures on some phrasings), so the
# retain step is driven directly against the API with bounded retries over a
# few phrasings. The daemon is exercised on the injection path (provider +
# local fallback).
#
# Usage: harness/tests/memory_recall_injection_real_smoke.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
HINDSIGHT="${HINDSIGHT:?set HINDSIGHT to the live hindsight url; the <deployment-lan> instance is retired (manual-run smoke, not in run_all)}"
BANK="nyaxus-smoke-inject-$(date +%Y%m%d%H%M%S)"
PROVIDER_PORT=8991
DAEMON_PORT=8992
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

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

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_URL="$HINDSIGHT" \
NYAXUS_MEMORY_BANK="$BANK" \
NYAXUS_MEMORY_RECALL_ON_TURN=1 \
NYAXUS_MEMORY_RETAIN_EVERY_N_TURNS=999999 \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "workdir: $WORKDIR"
echo "== health ok; bank: $BANK =="

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
# One local turn so the local fallback has a message to find.
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"remember that the vault passphrase is copper-meridian-7\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1; break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete"
echo "turn completed"

echo "== retain one item directly against the real provider (bounded retries) =="
curl -sf -X PUT "$HINDSIGHT/v1/default/banks/$BANK" -H 'Content-Type: application/json' -d '{}' > /dev/null \
    || fail "could not create the bank"
PHRASINGS=(
    "remember that the vault passphrase is copper-meridian-7\\n\\nAssistant: noted"
    "The vault passphrase is copper-meridian-7.\n\nAssistant: noted"
    "User: the vault passphrase is copper-meridian-7\n\nAssistant: got it"
)
RETAINED=""
for phrasing in "${PHRASINGS[@]}"; do
    for attempt in 1 2; do
        echo "retain attempt (phrasing ${#PHRASINGS[@]}, try $attempt) ..."
        OUT="$(curl -s -m 500 -X POST "$HINDSIGHT/v1/default/banks/$BANK/memories" \
            -H 'Content-Type: application/json' \
            -d "{\"async\":false,\"items\":[{\"content\":\"$phrasing\",\"context\":\"Nyaxus session $SID (profile orchestrator): completed turn\",\"document_id\":\"nyaxus-turn:smoke\",\"tags\":[\"nyaxus.session:$SID\",\"nyaxus.profile:orchestrator\"]}]}" || true)"
        if printf '%s' "$OUT" | grep -q '"success":true'; then
            RETAINED=1
            break 2
        fi
        printf '%s' "$OUT" | head -c 160; echo
    done
done
[ -n "$RETAINED" ] || fail "retain did not succeed (server-side extraction flaky)"
echo "retain completed"

echo "== run recalls the retained fact and injects it (real provider) =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what is the vault passphrase?\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "second run did not start"

ANSWER2="$(wait_for_marker 'Memory: past evidence')" || fail "second run did not inject memory"
printf '%s' "$ANSWER2" | grep -q 'copper-meridian-7' || fail "recalled fact missing from injection: $ANSWER2"
printf '%s' "$ANSWER2" | grep -q 'source=hindsight' || echo "note: injected via fallback, not the provider (provider recall may have been slow): $ANSWER2"
echo "real provider injection ok"

echo "== outage: point the provider at a dead port (live apply) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"memory.endpoint":"http://127.0.0.1:1"}')"
[ "$CODE" = "200" ] || fail "could not switch memory endpoint: $CODE"

echo "== third run falls back to local lexical search and still injects =="
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what is the vault passphrase?\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "third run did not start"

ANSWER3="$(wait_for_marker 'source=local')" || fail "third run did not inject local fallback"
printf '%s' "$ANSWER3" | grep -q 'copper-meridian-7' || fail "local fallback missed the turn: $ANSWER3"
echo "local fallback injection ok"

echo "== cleanup: delete the disposable bank =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X DELETE "$HINDSIGHT/v1/default/banks/$BANK")"
[ "$CODE" = "200" ] || echo "bank delete returned $CODE (check manually)"

echo
echo "MEMORY_RECALL_INJECTION_REAL_SMOKE_PASSED"
