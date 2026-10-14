#!/usr/bin/env bash
# message_delete_test.sh - destructive user-message delete semantics.
#
# Covers:
#   - deleting a user message removes it, its response, and the whole tail
#   - a sibling branch under the parent survives the delete, with the active
#     projection unchanged
#   - deleting the active branch promotes the newest surviving sibling to
#     active (the chat stays on a branch, not the bare parent)
#   - deleting the last active user message empties the session
#   - deleting an unknown message 404s
#
# Usage: harness/tests/message_delete_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8920
DAEMON_PORT=8921
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

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > mock.log 2>&1 &
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

session_json() { curl -sf "$BASE/sessions/$1"; }

count_messages() {
    session_json "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            process.stdout.write(String((JSON.parse(d).messages || []).length));
        });'
}

user_id_by_content() {
    session_json "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const m = (JSON.parse(d).messages || []).find((x) => x.role === "user" && x.content === process.argv[1]);
            process.stdout.write(m ? String(m.id) : "");
        });' "$2"
}

active_contents() {
    session_json "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const rows = (JSON.parse(d).messages || []).filter((m) => m.is_active).map((m) => m.content);
            process.stdout.write(rows.join("|"));
        });'
}

wait_run() {
    local done=""
    for _ in $(seq 1 75); do
        if session_json "$1" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            done=1
            break
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run in $1 did not complete"
}

send() {
    local run
    run="$(curl -sf -X POST "$BASE/sessions/$1/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$1\",\"message\":\"$2\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start"
    wait_run "$1"
}

new_session() {
    local sid
    sid="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "no session id"
    printf '%s' "$sid"
}

echo "== build a two-turn conversation =="
S1="$(new_session)"
send "$S1" "first"
send "$S1" "second"
COUNT="$(count_messages "$S1")"
[ "$COUNT" = "4" ] || fail "expected 4 messages, got $COUNT"
U1="$(user_id_by_content "$S1" "first")"
[ -n "$U1" ] || fail "could not find the first user message"
echo "4 messages, first user id $U1"

echo "== delete the first user message: the whole tail goes =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$S1/messages/$U1/delete")"
[ "$CODE" = "200" ] || fail "delete returned $CODE, want 200"
COUNT="$(count_messages "$S1")"
[ "$COUNT" = "0" ] || fail "expected 0 messages after delete, got $COUNT"
echo "tail removed"

echo "== build a branched conversation (edit creates a sibling) =="
S2="$(new_session)"
send "$S2" "first"
U1="$(user_id_by_content "$S2" "first")"
[ -n "$U1" ] || fail "could not find the first user message"
EDIT="$(curl -sf -X POST "$BASE/sessions/$S2/messages/$U1" -H 'Content-Type: application/json' \
    -d '{"content":"first-edited"}' | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$EDIT" ] || fail "edit did not start a run"
wait_run "$S2"
COUNT="$(count_messages "$S2")"
[ "$COUNT" = "4" ] || fail "expected 4 messages after edit (two branches of two), got $COUNT"
ACTIVE="$(active_contents "$S2")"
# The mock provider echoes the injected system blocks ([CLOCK], [PREV
# SESSION]) as its reply, so the assistant row carries them. The clock sits
# at the volatile tail now, so only assert the echo reached the provider.
case "$ACTIVE" in
    "first-edited|system: "*) ;;
    *) fail "active path is '$ACTIVE', want 'first-edited|system: ...'" ;;
esac
echo "branch built; active path: $ACTIVE"

echo "== delete the active branch: the surviving sibling is promoted =="
UA="$(user_id_by_content "$S2" "first-edited")"
[ -n "$UA" ] || fail "could not find the edited user message"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$S2/messages/$UA/delete")"
[ "$CODE" = "200" ] || fail "delete returned $CODE, want 200"
COUNT="$(count_messages "$S2")"
[ "$COUNT" = "2" ] || fail "expected 2 messages after delete, got $COUNT"
ACTIVE="$(active_contents "$S2")"
case "$ACTIVE" in
    "first|system: "*) ;;
    *) fail "sibling was not promoted to active: '$ACTIVE', want 'first|system: ...'" ;;
esac
echo "sibling promoted to active: $ACTIVE"

echo "== delete the last active user message: session empties =="
U1="$(user_id_by_content "$S2" "first")"
[ -n "$U1" ] || fail "could not find the promoted user message"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$S2/messages/$U1/delete")"
[ "$CODE" = "200" ] || fail "delete returned $CODE, want 200"
COUNT="$(count_messages "$S2")"
[ "$COUNT" = "0" ] || fail "expected 0 messages after delete, got $COUNT"
echo "session emptied"

echo "== delete the inactive branch: siblings survive, projection unchanged =="
S3="$(new_session)"
send "$S3" "first"
U1="$(user_id_by_content "$S3" "first")"
[ -n "$U1" ] || fail "could not find the first user message"
EDIT="$(curl -sf -X POST "$BASE/sessions/$S3/messages/$U1" -H 'Content-Type: application/json' \
    -d '{"content":"first-edited"}' | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$EDIT" ] || fail "edit did not start a run"
wait_run "$S3"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$S3/messages/$U1/delete")"
[ "$CODE" = "200" ] || fail "delete returned $CODE, want 200"
COUNT="$(count_messages "$S3")"
[ "$COUNT" = "2" ] || fail "expected 2 messages after delete, got $COUNT"
ACTIVE="$(active_contents "$S3")"
case "$ACTIVE" in
    "first-edited|system: "*) ;;
    *) fail "active path changed after deleting the inactive branch: '$ACTIVE'" ;;
esac
echo "sibling branch preserved"

echo "== unknown message id must 404 =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$S2/messages/99999999/delete")"
[ "$CODE" = "404" ] || fail "unknown delete returned $CODE, want 404"
echo "404 on unknown"

echo
echo "MESSAGE_DELETE_TEST_PASSED"
