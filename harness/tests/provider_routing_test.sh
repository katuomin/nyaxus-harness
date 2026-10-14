#!/usr/bin/env bash
# provider_routing_test.sh - e2e: a profile's named provider routes the run.
#
# Two mock providers run side by side: A is the daemon's default provider
# (global provider.* settings), B is a named provider (connection). A profile
# references B. The mocks record every request they receive, so the test can
# assert against the actual wire payload:
#
#   - a control run on the default profile hits A with A's model
#   - a run on the named-provider profile hits B, not A
#   - the request carries the named provider's model
#   - common params (temperature, top_p, seed) are present
#   - a custom passthrough param (dry_multiplier) is present verbatim
#   - the named provider's API key is sent as the Bearer token
#   - a profile's base_model and temperature override the connection
#   - run.started carries the named provider's context_window
#
# Usage: harness/tests/provider_routing_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_A_PORT=8911
MOCK_B_PORT=8912
DAEMON_PORT=8913
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

MOCK_PORT="$MOCK_A_PORT" MODEL_NAME="mock-model-a" \
    "$NODE" "$ROOT/harness/tests/mock_routing_provider.mjs" > mock_a.log 2>&1 &
APID=$!
MOCK_PORT="$MOCK_B_PORT" MODEL_NAME="mock-model-b" \
    "$NODE" "$ROOT/harness/tests/mock_routing_provider.mjs" > mock_b.log 2>&1 &
BPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_A_PORT/v1" --model mock-model-a > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$APID" "$BPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

# Field of the last recorded chat-completion request on a mock instance.
last_field() {
    curl -sf "http://127.0.0.1:$1/stats" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const v = (JSON.parse(d).last_request || {})[process.argv[1]];
            process.stdout.write(typeof v === "object" ? JSON.stringify(v) : String(v == null ? "" : v));
        });' "$2"
}

# Payload of the newest run.started event in a session snapshot.
last_started() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const started = (s.events || []).filter((e) => e.event_type === "run.started");
            const last = started[started.length - 1];
            process.stdout.write(last ? JSON.stringify(last.payload || {}) : "{}");
        });'
}

last_auth() {
    curl -sf "http://127.0.0.1:$1/stats" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            process.stdout.write(String(JSON.parse(d).last_auth == null ? "" : JSON.parse(d).last_auth));
        });'
}

completions() {
    curl -sf "http://127.0.0.1:$1/stats" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => { process.stdout.write(String(JSON.parse(d).completions)); });'
}

run_session() {
    local sid
    sid="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
        -d "{\"profile\":\"$1\"}" | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "no session id for profile $1"
    local run
    run="$(curl -sf -X POST "$BASE/sessions/$sid/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$sid\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start for profile $1"
    local done=""
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            done=1
            break
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run for profile $1 did not complete"
}

echo "== control: default profile hits the default provider (A) =="
run_session "orchestrator"
[ "$(completions "$MOCK_A_PORT")" = "1" ] || fail "A expected 1 completion, got $(completions "$MOCK_A_PORT")"
[ "$(completions "$MOCK_B_PORT")" = "0" ] || fail "B was contacted on the default profile"
[ "$(last_field "$MOCK_A_PORT" model)" = "mock-model-a" ] || fail "A request model is $(last_field "$MOCK_A_PORT" model), want mock-model-a"
echo "default provider routed"

echo "== create a named provider (connection B) with common + custom params =="
curl -sf -X POST "$BASE/providers" -H 'Content-Type: application/json' \
    -d '{"name":"conn-b","endpoint":"http://127.0.0.1:8912/v1","model":"named-model","api_key":"sekrit","params":{"temperature":0.3,"top_p":0.95,"seed":42,"context_window":4096,"dry_multiplier":0.8}}' > /dev/null \
    || fail "provider create failed"
echo "conn-b created"

echo "== create a profile referencing conn-b (no overrides) =="
curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"routed","name":"routed","provider":"conn-b"}' > /dev/null \
    || fail "profile create failed"
echo "routed created"

echo "== run on the named-provider profile =="
run_session "routed"
[ "$(completions "$MOCK_B_PORT")" = "1" ] || fail "B expected 1 completion, got $(completions "$MOCK_B_PORT")"
[ "$(completions "$MOCK_A_PORT")" = "1" ] || fail "A completion count changed (got $(completions "$MOCK_A_PORT")), default provider was contacted"
[ "$(last_field "$MOCK_B_PORT" model)" = "named-model" ] || fail "request model is $(last_field "$MOCK_B_PORT" model), want named-model"
[ "$(last_field "$MOCK_B_PORT" temperature)" = "0.3" ] || fail "temperature is $(last_field "$MOCK_B_PORT" temperature), want 0.3"
[ "$(last_field "$MOCK_B_PORT" top_p)" = "0.95" ] || fail "top_p is $(last_field "$MOCK_B_PORT" top_p), want 0.95"
[ "$(last_field "$MOCK_B_PORT" seed)" = "42" ] || fail "seed is $(last_field "$MOCK_B_PORT" seed), want 42"
[ "$(last_field "$MOCK_B_PORT" dry_multiplier)" = "0.8" ] || fail "custom param dry_multiplier is $(last_field "$MOCK_B_PORT" dry_multiplier), want 0.8"
[ "$(last_auth "$MOCK_B_PORT")" = "Bearer sekrit" ] || fail "auth header is '$(last_auth "$MOCK_B_PORT")', want 'Bearer sekrit'"
echo "named provider routed with model, params, custom param, api key"

echo "== profile base_model + temperature override the connection =="
curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"overridden","name":"overridden","provider":"conn-b","base_model":"mock-model-b","temperature":0.1}' > /dev/null \
    || fail "profile create failed"
run_session "overridden"
[ "$(completions "$MOCK_B_PORT")" = "2" ] || fail "B expected 2 completions, got $(completions "$MOCK_B_PORT")"
[ "$(last_field "$MOCK_B_PORT" model)" = "mock-model-b" ] || fail "override model is $(last_field "$MOCK_B_PORT" model), want mock-model-b"
[ "$(last_field "$MOCK_B_PORT" temperature)" = "0.1" ] || fail "override temperature is $(last_field "$MOCK_B_PORT" temperature), want 0.1"
echo "profile overrides applied"

echo "== run.started carries the connection's context window =="
SID_C="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"routed"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_C" ] || fail "no session id for the context window check"
curl -sf -X POST "$BASE/sessions/$SID_C/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_C\",\"message\":\"hello\"}" > /dev/null \
    || fail "run did not start"
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_C" 2>/dev/null | grep -q '"event_type":"run.completed"'; then break; fi
    sleep 0.2
done
CTX="$(last_started "$SID_C" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => { process.stdout.write(String(JSON.parse(d).context_window == null ? "" : JSON.parse(d).context_window)); });')"
[ "$CTX" = "4096" ] || fail "run.started context_window is '$CTX', want 4096"
echo "run.started carries the connection's context window"

echo
echo "PROVIDER_ROUTING_TEST_PASSED"
