#!/usr/bin/env bash
# settings_test.sh - typed runtime settings (step 3 of the branching plan).
#
# Covers:
#   - the full typed registry (50 settings) with provenance sources
#   - the provider check endpoint (valid endpoint lists models + ctx, unreachable -> 502)
#   - CLI > env > saved > default precedence
#   - live apply for non-restart settings
#   - validation errors (unknown key, wrong type, out of range)
#   - all-or-nothing persistence (an invalid entry persists nothing)
#   - write-only secret masking (plaintext never returned)
#   - persistence across a daemon restart (saved layer survives)
#
# Usage: harness/tests/settings_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
DAEMON_PORT=8942
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1"; exit 1; }

# entry <json> <key> -> value \t saved \t source \t type \t secret \t active \t restart
entry() {
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const key = process.argv[1];
            const s = JSON.parse(d);
            const e = (s.settings || []).find((x) => x.key === key);
            if (!e) { process.stdout.write("MISSING"); return; }
            const f = (v) => (v === null ? "null"
                : typeof v === "string" ? v : JSON.stringify(v));
            process.stdout.write([
                f(e.value), f(e.saved), e.source, e.type,
                e.secret ? "secret" : "plain",
                e.active ? "active" : "inactive",
                e.restart_required ? "restart" : "live",
            ].join("\u001f"));
        });' "$2"
}

# check <json> <key> <field> <expected>
check() {
    local row
    row="$(entry "$1" "$2")"
    local value saved source type secret active restart
    IFS=$'\u001f' read -r value saved source type secret active restart <<<"$row"
    local actual
    case "$3" in
        value) actual="$value" ;;
        saved) actual="$saved" ;;
        source) actual="$source" ;;
        type) actual="$type" ;;
        secret) actual="$secret" ;;
        active) actual="$active" ;;
        restart) actual="$restart" ;;
        *) actual="" ;;
    esac
    [ "$actual" = "$4" ] || fail "$2.$3: expected '$4', got '$actual'"
}

status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

start_daemon() {
    NYAXUS_TEMPERATURE=0.3 "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:19999/v1" --model mock-model > daemon.log 2>&1 &
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

echo "== registry shape =="
GET="$(curl -sf "$BASE/settings")"
COUNT="$(printf '%s' "$GET" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        process.stdout.write(String(JSON.parse(d).settings.length));
    });')"
[ "$COUNT" = "58" ] || fail "expected 58 settings, got $COUNT"
echo "registry shape verified"

echo "== provenance at startup (CLI > env > default) =="
check "$GET" port value "$DAEMON_PORT"
check "$GET" port source cli
check "$GET" port restart restart
check "$GET" provider value openai
check "$GET" provider source cli
check "$GET" provider.model value mock-model
check "$GET" provider.model source cli
check "$GET" provider.temperature value 0.3
check "$GET" provider.temperature source env
check "$GET" tool.timeout_ms value 60000
check "$GET" tool.timeout_ms saved null
check "$GET" tool.timeout_ms source default
check "$GET" tool.timeout_ms type integer
check "$GET" password value null
check "$GET" password secret secret
check "$GET" auxiliary.endpoint active inactive
echo "provenance and typing ok"

echo "== live apply: PUT a non-restart setting =="
PUT="$(curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"tool.timeout_ms":42000}')"
check "$PUT" tool.timeout_ms value 42000
check "$PUT" tool.timeout_ms saved 42000
check "$PUT" tool.timeout_ms source saved
echo "live apply ok"

echo "== validation errors =="
CODE="$(status -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"bogus.key":1}')"
[ "$CODE" = "400" ] || fail "unknown setting: expected 400, got $CODE"
BODY="$(curl -s -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"bogus.key":1}')"
printf '%s' "$BODY" | grep -q "unknown setting" || fail "unknown setting message missing"

CODE="$(status -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"port":"8080"}')"
[ "$CODE" = "400" ] || fail "string port: expected 400, got $CODE"

CODE="$(status -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"port":70000}')"
[ "$CODE" = "400" ] || fail "port out of range: expected 400, got $CODE"
BODY="$(curl -s -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"port":70000}')"
printf '%s' "$BODY" | grep -q "between 1 and 65535" || fail "port range message missing"

CODE="$(status -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"provider":"bogus"}')"
[ "$CODE" = "400" ] || fail "bad provider: expected 400, got $CODE"

CODE="$(status -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"auxiliary.vision":"yes"}')"
[ "$CODE" = "400" ] || fail "string boolean: expected 400, got $CODE"

CODE="$(status -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"provider.temperature":3.5}')"
[ "$CODE" = "400" ] || fail "temperature out of range: expected 400, got $CODE"
echo "validation ok"

echo "== PUT route: the write handler, not a silent GET =="
# The old catch-all routed PUT into the GET table, so PUT 200'd as a read
# and wrote nothing. PUT now serves put_settings.
CODE="$(status -X PUT "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"provider.temperature":0.45}')"
[ "$CODE" = "200" ] || fail "PUT settings: expected 200, got $CODE"
GET="$(curl -sf "$BASE/settings")"
check "$GET" provider.temperature value 0.45
check "$GET" provider.temperature saved 0.45
CODE="$(status -X PUT "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"bogus.key":1}')"
[ "$CODE" = "400" ] || fail "PUT unknown setting: expected 400, got $CODE"
echo "put ok"

echo "== wrong method on a known path: 405 with Allow =="
CODE="$(status -X DELETE "$BASE/settings")"
[ "$CODE" = "405" ] || fail "DELETE settings: expected 405, got $CODE"
HEADERS="$(curl -s -D - -o /dev/null -X DELETE "$BASE/settings")"
printf '%s' "$HEADERS" | grep -qi "^Allow:.*GET" || fail "405 missing Allow: GET"
echo "405 ok"

echo "== all-or-nothing persistence =="
CODE="$(status -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"summary.include_reasoning":true,"bogus.key":1}')"
[ "$CODE" = "400" ] || fail "mixed valid/invalid: expected 400, got $CODE"
GET="$(curl -sf "$BASE/settings")"
check "$GET" summary.include_reasoning saved null
echo "atomicity ok"

echo "== write-only secret masking =="
PUT="$(curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"provider.api_key":"sekrit-key"}')"
printf '%s' "$PUT" | grep -q "sekrit-key" && fail "plaintext secret leaked in response"
check "$PUT" provider.api_key value '••••'
check "$PUT" provider.api_key saved '••••'
GET="$(curl -sf "$BASE/settings")"
printf '%s' "$GET" | grep -q "sekrit-key" && fail "plaintext secret leaked in GET"
check "$GET" provider.api_key saved '••••'
echo "masking ok"

echo "== saved layer survives a restart =="
PUT="$(curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"provider.temperature":0.9}')"
check "$PUT" provider.temperature saved 0.9
stop_daemon
sleep 0.5
start_daemon || fail "daemon did not come up after restart (see $WORKDIR/daemon.log)"
GET="$(curl -sf "$BASE/settings")"
check "$GET" tool.timeout_ms value 42000
check "$GET" tool.timeout_ms saved 42000
check "$GET" tool.timeout_ms source saved
check "$GET" provider.api_key saved '••••'
# env (0.3) must beat the saved value (0.9)
check "$GET" provider.temperature value 0.3
check "$GET" provider.temperature source env
check "$GET" provider.temperature saved 0.9
echo "restart persistence ok"

echo "== provider check: endpoint probe + model list =="
MOCK_PORT=8943 "$NODE" "$ROOT/harness/tests/mock_provider.mjs" > mock.log 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 50); do
    curl -sf "http://127.0.0.1:8943/v1/models" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "http://127.0.0.1:8943/v1/models" > /dev/null 2>&1 || fail "mock provider did not come up"
CHECK="$(curl -sf -X POST "$BASE/provider/check" -H 'Content-Type: application/json' \
    -d '{"endpoint":"http://127.0.0.1:8943/v1"}')"
printf '%s' "$CHECK" | grep -q '"ok":true' || fail "provider check: ok flag missing"
printf '%s' "$CHECK" | grep -q '"id":"mock-model"' || fail "provider check: model id missing"
printf '%s' "$CHECK" | grep -q '"context_window":4096' || fail "provider check: context_window missing"
CODE="$(status -X POST "$BASE/provider/check" -H 'Content-Type: application/json' \
    -d '{"endpoint":"http://127.0.0.1:1/v1"}')"
[ "$CODE" = "502" ] || fail "unreachable endpoint: expected 502, got $CODE"
CODE="$(status -X POST "$BASE/provider/check" -H 'Content-Type: application/json' \
    -d '{}')"
[ "$CODE" = "400" ] || fail "missing endpoint: expected 400, got $CODE"
kill "$MOCK_PID" > /dev/null 2>&1 || true
wait "$MOCK_PID" > /dev/null 2>&1 || true
echo "provider check ok"

echo "SETTINGS_TEST_PASSED"
