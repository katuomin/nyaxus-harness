#!/usr/bin/env bash
# infra_test.sh - the infrastructure observability snapshot works.
#
# Asserts:
#   - GET /api/v1/infra returns 200 with the host / gpu / containers /
#     providers sections and the docker_available flag
#   - the daemon reports its own box (mem_total_mb > 0)
#   - the primary provider LED is present; the test points it at a dead
#     endpoint, so it must read online=false (json keys serialize sorted)
#
# Usage: harness/tests/infra_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
DAEMON_PORT=9042
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

e2e_port_free "$DAEMON_PORT"

echo "== launch daemon =="
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:19999/v1" --model mock-model \
    > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null 2>&1 || fail "daemon did not come up (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "healthy"

echo "== GET /api/v1/infra =="
STATUS="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/infra")"
[ "$STATUS" = "200" ] || fail "infra returned $STATUS, want 200"
BODY="$(curl -sf "$BASE/infra")"

printf '%s' "$BODY" | grep -q '"host"' || fail "host section missing"
printf '%s' "$BODY" | grep -q '"gpu"' || fail "gpu section missing"
printf '%s' "$BODY" | grep -q '"containers"' || fail "containers section missing"
printf '%s' "$BODY" | grep -q '"providers"' || fail "providers section missing"
printf '%s' "$BODY" | grep -q '"docker_available"' || fail "docker_available flag missing"
printf '%s' "$BODY" | grep -q '"collected_at_ms"' || fail "collected_at_ms missing"

# Host stats: the daemon must report its own box's total memory.
printf '%s' "$BODY" | grep -Eq '"mem_total_mb":[1-9]' \
    || fail "mem_total_mb not reported"

# Provider LED: the primary is configured against a dead endpoint, so the
# entry exists and reads offline (json keys serialize in sorted order).
printf '%s' "$BODY" | grep -Eq '"name":"primary","online":false' \
    || fail "primary led missing or not offline"
printf '%s' "$BODY" | grep -Eq '"endpoint":"http://127.0.0.1:19999/v1"' \
    || fail "primary endpoint not echoed"

echo
echo "INFRA_TEST_PASSED"
