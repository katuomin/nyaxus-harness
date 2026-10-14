#!/usr/bin/env bash
# End-to-end check for the cross-session search route (GET /api/v1/search).
#
# A text-only mock provider serves two sessions, each with a user message
# carrying the shared phrase "zephyr protocol". The route must find the
# phrase in both sessions (newest first), with per-hit session metadata and
# a snippet centered on the match. Also asserts the 400 contract for a
# missing q and an empty result set for a non-matching query.
#
# Usage: harness/tests/search_all_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8931
DAEMON_PORT=8932
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_search_all_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

wait_run_done() {
    local sid="$1"
    local done=""
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            done=1
            break
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run in session $sid did not complete (see $WORKDIR/daemon.log)"
}

create_and_run() {
    local message="$1"
    local sid
    sid="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "no session id"
    local run
    run="$(curl -sf -X POST "$BASE/sessions/$sid/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$sid\",\"message\":\"$message\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start in $sid"
    wait_run_done "$sid"
    echo "$sid"
}

echo "== session A (zephyr protocol note) =="
SIDA="$(create_and_run "the zephyr protocol note lives in session A")"
echo "$SIDA"

echo "== session B (zephyr protocol copy) =="
SIDB="$(create_and_run "the zephyr protocol copy lives in session B")"
echo "$SIDB"

echo "== missing q -> 400 =="
STATUS="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/search")"
[ "$STATUS" = "400" ] || fail "expected 400 for missing q, got $STATUS"
echo "ok"

echo "== no-match query -> empty result set =="
NOMATCH="$(curl -sf "$BASE/search?q=nomatchxyz")"
printf '%s' "$NOMATCH" | grep -q '"matches":\[\]' \
    || fail "expected an empty match list: $NOMATCH"
printf '%s' "$NOMATCH" | grep -q '"truncated":false' \
    || fail "no-match result must report truncated=false: $NOMATCH"
echo "ok"

echo "== cross-session search (zephyr protocol) =="
RESULTS="$(curl -sf "$BASE/search?q=zephyr+protocol")"
echo "$RESULTS"
HITS="$(printf '%s' "$RESULTS" | grep -o '"message_id"' | wc -l | tr -d ' ')"
echo "hits: $HITS (want 2)"
[ "$HITS" = "2" ] || fail "expected 2 hits across the two sessions, got $HITS"

# Both sessions must be represented, each with its own session_id.
printf '%s' "$RESULTS" | grep -q "\"session_id\":\"$SIDA\"" \
    || fail "session A missing from the results"
printf '%s' "$RESULTS" | grep -q "\"session_id\":\"$SIDB\"" \
    || fail "session B missing from the results"
# The hits are the user messages carrying the phrase (role metadata present).
printf '%s' "$RESULTS" | grep -q '"role":"user"' \
    || fail "hits must carry the matching message's role"
# Snippets are centered on the match.
printf '%s' "$RESULTS" | grep -q 'zephyr protocol' \
    || fail "snippets must carry the matched phrase"
# Newest session first: B was created after A, so B leads.
FIRST_SID="$(printf '%s' "$RESULTS" | sed -n 's/.*"matches":\[{"created_at":"[^"]*","message_id":[0-9]*,"role":"[^"]*","session_id":"\([^"\]*\)".*/\1/p')"
[ "$FIRST_SID" = "$SIDB" ] || fail "results must be newest-session-first (want $SIDB first, got $FIRST_SID)"
printf '%s' "$RESULTS" | grep -q '"truncated":false' \
    || fail "small store must report truncated=false"

echo "SEARCH_ALL_TEST_PASSED"
