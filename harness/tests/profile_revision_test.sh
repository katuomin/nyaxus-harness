#!/usr/bin/env bash
# profile_revision_test.sh - profile revisions and the drift guard (step 4).
#
# Covers:
#   - profile edits bump the revision counter (create = 1, each edit = +1)
#   - the first run of a session pins the revision without "adopting" it
#   - a run that starts after a profile edit adopts the new revision and
#     records exactly which fields drifted (persona, model, temperature)
#   - an unchanged re-run is not an adoption
#   - a fresh session pins the current revision without adoption
#   - switching profiles is an adoption that records the profile change
#   - the session's pin survives a daemon restart (persistence)
#
# Usage: harness/tests/profile_revision_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8951
DAEMON_PORT=8952
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1"; exit 1; }

# field <json> <key> -> the field's string value (empty when absent)
field() {
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const o = JSON.parse(d);
            const v = o[process.argv[1]];
            process.stdout.write(v === undefined || v === null ? "" : String(v));
        });' "$2"
}

# last_started <session-json> -> payload of the newest run.started event
last_started() {
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const started = (s.events || []).filter((e) => e.event_type === "run.started");
            const last = started[started.length - 1];
            process.stdout.write(last ? JSON.stringify(last.payload || {}) : "{}");
        });'
}

wait_run() {
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$1" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

send() { # send <sid> <message>
    curl -sf -X POST "$BASE/sessions/$1/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$1\",\"message\":\"$2\"}" > /dev/null
}

start_daemon() {
    MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > mock.log 2>&1 &
    MPID=$!
    "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
    for _ in $(seq 1 50); do
        curl -sf "$BASE/health" > /dev/null 2>&1 && return 0
        sleep 0.2
    done
    return 1
}

stop_daemon() { taskkill //F //IM nyaxus_daemon.exe > /dev/null 2>&1 || true; }

stop_daemon
start_daemon || fail "daemon did not come up (see $WORKDIR/daemon.log)"
trap 'stop_daemon' EXIT

echo "== create profile at revision 1 =="
P="$(curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"drift-a","name":"drift a","description":"drift test","identity_prompt":"You are A.","base_model":"mock-model","temperature":0.5}')"
[ "$(field "$P" revision)" = "1" ] || fail "create: expected revision 1, got '$(field "$P" revision)'"
echo "revision 1 on create"

echo "== first run pins the revision without adopting =="
SID_A="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"drift-a"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_A" ] || fail "no session id"
send "$SID_A" "hello" || fail "run 1 rejected"
wait_run "$SID_A" || fail "run 1 did not complete (see $WORKDIR/daemon.log)"
P1="$(last_started "$(curl -sf "$BASE/sessions/$SID_A")")"
[ "$(field "$P1" profile_revision)" = "1" ] || fail "run1 revision: '$(field "$P1" profile_revision)'"
[ "$(field "$P1" profile_adopted)" = "false" ] || fail "run1 adopted: '$(field "$P1" profile_adopted)'"
[ -z "$(field "$P1" profile_drift)" ] || fail "run1 drift: '$(field "$P1" profile_drift)'"
[ -n "$(field "$P1" profile_fingerprint)" ] || fail "run1 fingerprint missing"
echo "pin without adoption"

echo "== profile edit bumps the revision =="
P2="$(curl -sf -X POST "$BASE/profiles/drift-a" -H 'Content-Type: application/json' \
    -d '{"name":"drift a","description":"drift test","identity_prompt":"You are B.","base_model":"mock-model","temperature":0.9}')"
[ "$(field "$P2" revision)" = "2" ] || fail "update: expected revision 2, got '$(field "$P2" revision)'"
echo "revision 2 on edit"

echo "== next run adopts the new revision with drift recorded =="
send "$SID_A" "hello again" || fail "run 2 rejected"
wait_run "$SID_A" || fail "run 2 did not complete"
P3="$(last_started "$(curl -sf "$BASE/sessions/$SID_A")")"
[ "$(field "$P3" profile_revision)" = "2" ] || fail "run2 revision: '$(field "$P3" profile_revision)'"
[ "$(field "$P3" profile_adopted)" = "true" ] || fail "run2 adopted: '$(field "$P3" profile_adopted)'"
DRIFT="$(field "$P3" profile_drift)"
printf '%s' "$DRIFT" | grep -q "identity_prompt" || fail "drift missing identity_prompt: $DRIFT"
printf '%s' "$DRIFT" | grep -q "temperature" || fail "drift missing temperature: $DRIFT"
echo "adoption recorded: $DRIFT"

echo "== unchanged re-run is not an adoption =="
send "$SID_A" "hello once more" || fail "run 3 rejected"
wait_run "$SID_A" || fail "run 3 did not complete"
P4="$(last_started "$(curl -sf "$BASE/sessions/$SID_A")")"
[ "$(field "$P4" profile_adopted)" = "false" ] || fail "run3 adopted: '$(field "$P4" profile_adopted)'"
echo "no false adoption"

echo "== fresh session pins the current revision without adoption =="
SID_B="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"drift-a"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_B" ] || fail "no second session id"
send "$SID_B" "hello" || fail "run B rejected"
wait_run "$SID_B" || fail "run B did not complete"
PB="$(last_started "$(curl -sf "$BASE/sessions/$SID_B")")"
[ "$(field "$PB" profile_revision)" = "2" ] || fail "runB revision: '$(field "$PB" profile_revision)'"
[ "$(field "$PB" profile_adopted)" = "false" ] || fail "runB adopted: '$(field "$PB" profile_adopted)'"
echo "fresh pin ok"

echo "== switching profiles is an adoption with profile drift =="
curl -sf -X POST "$BASE/sessions/$SID_A/profile" -H 'Content-Type: application/json' \
    -d '{"profile":"orchestrator"}' > /dev/null || fail "profile switch rejected"
send "$SID_A" "one more" || fail "run 4 rejected"
wait_run "$SID_A" || fail "run 4 did not complete"
P5="$(last_started "$(curl -sf "$BASE/sessions/$SID_A")")"
[ "$(field "$P5" profile_adopted)" = "true" ] || fail "run4 adopted: '$(field "$P5" profile_adopted)'"
printf '%s' "$(field "$P5" profile_drift)" | grep -q "profile" || \
    fail "run4 drift missing profile: $(field "$P5" profile_drift)"
echo "switch adoption ok"

echo "== pin survives a daemon restart =="
stop_daemon
sleep 0.5
start_daemon || fail "daemon did not come up after restart (see $WORKDIR/daemon.log)"
send "$SID_A" "after restart" || fail "run 5 rejected"
wait_run "$SID_A" || fail "run 5 did not complete"
P6="$(last_started "$(curl -sf "$BASE/sessions/$SID_A")")"
[ "$(field "$P6" profile_adopted)" = "false" ] || \
    fail "run5 adopted after restart: '$(field "$P6" profile_adopted)'"
echo "pin persisted"

echo "PROFILE_REVISION_TEST_PASSED"
