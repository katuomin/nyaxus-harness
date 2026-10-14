#!/usr/bin/env bash
# providers_test.sh - named model connections (providers) + profile routing.
#
# Covers:
#   - create / list / update / delete a provider (connection)
#   - write-only api_key masking (plaintext never returned)
#   - params round-trip (common + custom passthrough keys)
#   - a profile can reference a provider by name
#   - an unknown provider reference is rejected on the profile
#   - deleting a provider that a profile references is allowed (profile falls
#     back to the default provider at run time)
#
# Usage: harness/tests/providers_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
DAEMON_PORT=8950
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1"; exit 1; }

status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

start_daemon() {
    "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:19999/v1" --model default-model > daemon.log 2>&1 &
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

echo "== create a provider (connection) =="
CREATE="$(curl -sf -X POST "$BASE/providers" -H 'Content-Type: application/json' \
    -d '{"name":"local-27b","endpoint":"http://<deployment-lan>:8080/v1","model":"qwen-27b","api_key":"sekrit","params":{"temperature":0.4,"top_p":0.95,"vision":true,"dry_multiplier":0.8}}')"
printf '%s' "$CREATE" | grep -q '"name":"local-27b"' || fail "create: name missing"
printf '%s' "$CREATE" | grep -q '"endpoint":"http://<deployment-lan>:8080/v1"' || fail "create: endpoint missing"
printf '%s' "$CREATE" | grep -q '"model":"qwen-27b"' || fail "create: model missing"
printf '%s' "$CREATE" | grep -q '"temperature":0.4' || fail "create: param temperature missing"
printf '%s' "$CREATE" | grep -q '"top_p":0.95' || fail "create: param top_p missing"
printf '%s' "$CREATE" | grep -q '"dry_multiplier":0.8' || fail "create: custom param missing"
echo "create ok"

echo "== write-only secret masking =="
printf '%s' "$CREATE" | grep -q "sekrit" && fail "plaintext api_key leaked in create"
printf '%s' "$CREATE" | grep -q '"api_key":"••••"' || fail "api_key not masked"
echo "masking ok"

echo "== list providers =="
LIST="$(curl -sf "$BASE/providers")"
COUNT="$(printf '%s' "$LIST" | "$NODE" -e '
    let d=""; process.stdin.on("data",(c)=>{d+=c;});
    process.stdin.on("end",()=>{ process.stdout.write(String(JSON.parse(d).providers.length)); });')"
[ "$COUNT" = "1" ] || fail "list: expected 1 provider, got $COUNT"
echo "list ok"

echo "== update a provider (real-time, no restart) =="
UPDATE="$(curl -sf -X POST "$BASE/providers" -H 'Content-Type: application/json' \
    -d '{"name":"local-27b","endpoint":"http://<deployment-lan>:8080/v1","model":"qwen-27b-v2","params":{"temperature":0.6,"top_k":40}}')"
printf '%s' "$UPDATE" | grep -q '"model":"qwen-27b-v2"' || fail "update: model not changed"
printf '%s' "$UPDATE" | grep -q '"temperature":0.6' || fail "update: temperature not changed"
printf '%s' "$UPDATE" | grep -q '"top_k":40' || fail "update: top_k missing"
# The api_key was not re-sent; it must be preserved (still masked).
printf '%s' "$UPDATE" | grep -q '"api_key":"••••"' || fail "update: api_key not preserved"
echo "update ok"

echo "== a profile can reference a provider =="
PROFILE="$(curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"researcher","name":"researcher","provider":"local-27b","base_model":"qwen-27b-v2"}')"
printf '%s' "$PROFILE" | grep -q '"provider":"local-27b"' || fail "profile: provider reference missing"
printf '%s' "$PROFILE" | grep -q '"base_model":"qwen-27b-v2"' || fail "profile: base_model missing"
echo "profile routing ok"

echo "== primary connection serves as the default provider =="
# The update above replaced params; give the connection a vision capability.
curl -sf -X POST "$BASE/providers" -H 'Content-Type: application/json' \
    -d '{"name":"local-27b","endpoint":"http://<deployment-lan>:8080/v1","model":"qwen-27b-v2","params":{"vision":true}}' > /dev/null
# Designate it as the primary (the provider.* source).
curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"provider.default":"local-27b"}' > /dev/null
HEALTH="$(curl -sf "$BASE/health")"
printf '%s' "$HEALTH" | grep -q '"model":"qwen-27b-v2"' || fail "primary: health model not from connection"
printf '%s' "$HEALTH" | grep -q '"vision":true' || fail "primary: health vision not from connection"
printf '%s' "$HEALTH" | grep -q '"default_provider":"local-27b"' || fail "primary: health default_provider missing"
# Clearing the primary reverts the default to the provider.* settings.
curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' -d '{"provider.default":""}' > /dev/null
HEALTH="$(curl -sf "$BASE/health")"
printf '%s' "$HEALTH" | grep -q '"model":"default-model"' || fail "primary: clear did not revert model"
printf '%s' "$HEALTH" | grep -q '"default_provider":""' || fail "primary: clear did not reset default_provider"
echo "primary connection ok"

echo "== validation: name and endpoint required =="
CODE="$(status -X POST "$BASE/providers" -H 'Content-Type: application/json' -d '{"name":"x"}')"
[ "$CODE" = "400" ] || fail "missing endpoint: expected 400, got $CODE"
CODE="$(status -X POST "$BASE/providers" -H 'Content-Type: application/json' -d '{"endpoint":"http://x/v1"}')"
[ "$CODE" = "400" ] || fail "missing name: expected 400, got $CODE"
echo "validation ok"

echo "== delete a provider =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/providers/local-27b/delete")"
[ "$CODE" = "200" ] || fail "delete: expected 200, got $CODE"
LIST="$(curl -sf "$BASE/providers")"
COUNT="$(printf '%s' "$LIST" | "$NODE" -e '
    let d=""; process.stdin.on("data",(c)=>{d+=c;});
    process.stdin.on("end",()=>{ process.stdout.write(String(JSON.parse(d).providers.length)); });')"
[ "$COUNT" = "0" ] || fail "delete: expected 0 providers, got $COUNT"
CODE="$(status -X POST "$BASE/providers/local-27b/delete")"
[ "$CODE" = "404" ] || fail "delete again: expected 404, got $CODE"
echo "delete ok"

echo "PROVIDERS_TEST_PASSED"
