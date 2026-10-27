#!/usr/bin/env bash
# group_invite_test.sh - group chat invite e2e (docs/group-chat.md): a
# profile may be added to an existing conversation, upgrading a 1:1 session
# to a group (or growing a group) while the shared transcript survives.
#
# Asserts:
#   - a 1:1 session chats normally (no group.created, no speakers)
#   - POST /sessions/{id}/speakers {"add": profile} validates: unknown
#     profile 404, already-in-roster 400, missing field 400
#   - the invite appends the profile at the roster's end (the 1:1 agent
#     stays first), persists it, and lands one group.joined per invite
#     carrying the post-join roster - without faking a group.created
#   - the next send drives a group run whose turns speak in the joined
#     order, closes on unanimous abstention, and the pre-group history
#     stays on the same parent chain
#   - a second invite + run extends the same session again
#
# group.abstain is read-only: no approval gate anywhere in the test.
#
# Usage: harness/tests/group_invite_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9094
DAEMON_PORT=9099
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
MOCK="http://127.0.0.1:$MOCK_PORT"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# run_json <script> <arg> <url> <fail-message> - run a node assertion on
# stdin JSON (arg lands in process.argv[1]).
run_json() { "$NODE" -e "$1" "$2" < <(curl -sf "$3") || fail "$4"; }

e2e_port_free "$DAEMON_PORT"
e2e_port_free "$MOCK_PORT"

echo "== launch mock provider + daemon =="
MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_invite_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd "$WORKDIR" && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null 2>&1 || fail "daemon did not come up (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "healthy"

echo "== profiles: alice, bob, carol =="
for p in alice bob carol; do
    CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles" \
        -H 'Content-Type: application/json' \
        -d "{\"id\":\"$p\",\"name\":\"$(echo "$p" | sed 's/^./\U&/')\"}")"
    [ "$CODE" = "200" ] || fail "$p profile create returned $CODE"
done
echo "profiles created"

wait_run_done() {
    for _ in $(seq 1 200); do
        curl -sf "$BASE/sessions/$1" 2>/dev/null | grep -q '"event_type":"run.completed"' && return 0
        sleep 0.2
    done
    return 1
}

wait_group_completed() {
    for _ in $(seq 1 200); do
        curl -sf "$BASE/sessions/$1" 2>/dev/null | grep -q '"event_type":"group.completed"' && return 0
        sleep 0.2
    done
    return 1
}

# wait_group_done <sid> <rid> - wait for THIS run's group.completed (the
# session's ledger may still carry an older group.completed, so a plain
# existence grep would return early on a second group run).
wait_group_done() {
    for _ in $(seq 1 200); do
        if curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
            let parsed;
            try { parsed = JSON.parse(require("fs").readFileSync(0, "utf8")); }
            catch { process.exit(1); }
            const rid = process.argv[1];
            const events = parsed.events || [];
            process.exit(events.some((e) => e.event_type === "group.completed" && e.run_id === rid) ? 0 : 1);
        ' "$2" 2>/dev/null; then return 0; fi
        sleep 0.2
    done
    return 1
}

echo "== S1: a 1:1 session chats before any invite =="
SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
curl -sf "$BASE/sessions/$SID" | grep -q '"speakers":\[\]' \
    || fail "a 1:1 session must carry an empty speakers roster"
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"group.created"' \
    && fail "a 1:1 session must not carry group.created"

RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"solo hello\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "the 1:1 run did not start"
wait_run_done "$SID" || fail "the 1:1 run never completed"

echo "== S1: the invite route validates =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/speakers" \
    -H 'Content-Type: application/json' -d '{"add":"ghost"}')"
[ "$CODE" = "404" ] || fail "an unknown profile must be 404, got $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/speakers" \
    -H 'Content-Type: application/json' -d '{"add":"alice"}')"
[ "$CODE" = "400" ] || fail "the nominal profile is already in the roster (400), got $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/speakers" \
    -H 'Content-Type: application/json' -d '{}')"
[ "$CODE" = "400" ] || fail "a missing add field must be 400, got $CODE"
echo "validation ok"

echo "== S1: invite bob (1:1 -> group) =="
curl -sf -X POST "$BASE/sessions/$SID/speakers" -H 'Content-Type: application/json' \
    -d '{"add":"bob"}' | grep -q '"speakers":\["alice","bob"\]' \
    || fail "the invite response must carry the post-join roster"
curl -sf "$BASE/sessions/$SID" | grep -q '"speakers":\["alice","bob"\]' \
    || fail "the session payload must persist the joined roster"
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"group.joined"' \
    || fail "group.joined missing from the ledger"
curl -sf "$BASE/sessions/$SID" | grep -q '"speaker":"bob"' \
    || fail "group.joined must carry the joined speaker"
echo "invited"

echo "== S1: a converted session must not grow a group.created =="
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"group.created"' \
    && fail "a converted session must not grow a group.created"
echo "no fake birth"

echo "== S1: the next send is a group run over the retained history =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"now as a group\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "the group run did not start"
wait_group_done "$SID" "$RUN2" || fail "the group never completed (see $WORKDIR/daemon.log)"
curl -sf "$BASE/sessions/$SID" | grep -q '"termination":"unanimous abstention"' \
    || fail "the converted group must close on unanimous abstention"
# The transcript: solo user + solo reply + the group send + the r1 turns
# (alice, bob) + alice's r2 speech (abstentions leave no message rows) -
# one active parent chain.
run_json '
    const parsed = JSON.parse(require("fs").readFileSync(0, "utf8"));
    const messages = parsed.messages || [];
    if (messages.length !== 6) {
        console.error("expected 6 messages, got " + messages.length);
        process.exit(1);
    }
    let pid = 0;
    for (const m of messages) {
        if (!m.is_active) { console.error("inactive message: " + m.id); process.exit(1); }
        if (m.parent_id !== pid) { console.error("chain broken at " + m.id); process.exit(1); }
        pid = m.id;
    }
    const speakers = messages.map((m) => m.speaker || "");
    if (speakers[0] !== "" || speakers[1] !== "") {
        console.error("the 1:1 history must stay unattributed: " + JSON.stringify(speakers));
        process.exit(1);
    }
    const turns = speakers.slice(3).join(",");
    if (turns !== "alice,bob,alice") {
        console.error("the group turns must speak in the joined order, got: " + turns);
        process.exit(1);
    }
' "chain" "$BASE/sessions/$SID" "the converted transcript chain is wrong"
# The group framing must name the joined roster for the new speaker's turn.
run_json '
    const requests = require("fs").readFileSync(0, "utf8");
    const parsed = JSON.parse(requests);
    const bodies = parsed.map((r) => (r.messages || []).map((m) => String(m.content || "")).join("\n")).join("\n");
    if (!/\[GROUP\]/.test(bodies)) {
        console.error("no [GROUP] framing recorded");
        process.exit(1);
    }
    if (!/Bob/.test(bodies) || !/Alice/.test(bodies)) {
        console.error("the framing must name the joined roster");
        process.exit(1);
    }
' "framing" "$MOCK/requests" "the group framing must name the joined roster"
echo "group run over retained history ok"

echo "== S1: a second invite extends the same session =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/speakers" \
    -H 'Content-Type: application/json' -d '{"add":"bob"}')"
[ "$CODE" = "400" ] || fail "inviting an existing speaker twice must be 400, got $CODE"
curl -sf -X POST "$BASE/sessions/$SID/speakers" -H 'Content-Type: application/json' \
    -d '{"add":"carol"}' | grep -q '"speakers":\["alice","bob","carol"\]' \
    || fail "the second invite must extend the roster in order"
JOINED="$(curl -sf "$BASE/sessions/$SID" | grep -o '"event_type":"group.joined"' | wc -l)"
[ "$JOINED" = "2" ] || fail "expected 2 group.joined events, got $JOINED"

RUN3="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"three at the table\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "the second group run did not start"
wait_group_done "$SID" "$RUN3" || fail "the second group never completed (see $WORKDIR/daemon.log)"
run_json '
    const parsed = JSON.parse(require("fs").readFileSync(0, "utf8"));
    const messages = parsed.messages || [];
    // 6 + the third send + the three r1 turns (r2 is all abstentions).
    if (messages.length !== 10) {
        console.error("expected 10 messages, got " + messages.length);
        process.exit(1);
    }
    const tail = messages.map((m) => m.speaker || "").slice(-3).join(",");
    if (tail !== "alice,bob,carol") {
        console.error("the three-speaker order is wrong: " + tail);
        process.exit(1);
    }
' "chain3" "$BASE/sessions/$SID" "the three-speaker transcript is wrong"
echo "second invite ok"

echo "GROUP_INVITE_TEST_PASSED"
