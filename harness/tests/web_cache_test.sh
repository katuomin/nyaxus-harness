#!/usr/bin/env bash
# End-to-end check for the web fetch resilience: the cache-served refetch.
# Run 1 fetches the fixture page through the local https fixture (the ledger
# records the ok row and persists the extracted text under spills/). Run 2
# fetches the SAME url with the fixture STOPPED - every live tier fails
# (native transport down; no escalation tiers configured), so the daemon
# returns the persisted copy with the original [src:N] id and a cache
# marker, and the ledger's ok row stays intact instead of being clobbered
# by the failure.
#
# Usage: harness/tests/web_cache_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9085
TLS_PORT=9446
DAEMON_PORT=9086
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
# The daemon is a native process: MSYS-style /tmp paths in its settings
# must be Windows paths (cygpath; empty pass-through where cygpath is
# absent - same conversion as web_tls_test.sh).
DAEMON_WORKDIR="$WORKDIR"
if command -v cygpath > /dev/null; then
    DAEMON_WORKDIR="$(cygpath -m "$WORKDIR")"
fi

# Fixture + CA (openssl when present; fall back to the dev-tree keys).
if command -v openssl > /dev/null; then
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 \
        -keyout key.pem -out ca.pem -nodes -days 3650 \
        -subj "/CN=nyaxus-cache-fixture" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" > /dev/null 2>&1
fi
[ -f ca.pem ] || { echo "FAIL: no fixture keys" >&2; exit 1; }

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$MOCK_PORT" \
CALLS='[{"name":"web.fetch","arguments":{"url":"https://127.0.0.1:9446/"}},{"text":"done"},{"name":"web.fetch","arguments":{"url":"https://127.0.0.1:9446/"}}]' \
    "$NODE" "$ROOT/harness/tests/mock_spill_provider.mjs" > mock.log 2>&1 &
MPID=$!
trap 'kill "$DPID" "$MPID" "$TPID" 2>/dev/null || true' EXIT
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
start_fixture() {
    "$NODE" "$ROOT/harness/tests/tls_fixture.mjs" "$TLS_PORT" \
        "$WORKDIR/ca.pem" "$WORKDIR/key.pem" > fixture.log 2>&1 &
    TPID=$!
}
start_fixture
sleep 1.5

curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d "{\"web.ca_bundle\":\"$DAEMON_WORKDIR/ca.pem\",\"web.firecrawl_endpoint\":\"\"}" > /dev/null \
    || fail "web.ca_bundle rejected"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

run_turn() {
    # run_turn <message>: one scripted run through the mock provider.
    curl -s -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$1\"}" > /dev/null
}

echo "== run 1: live fetch (fixture up) =="
run_turn "fetch https://127.0.0.1:$TLS_PORT/ page one"
for _ in $(seq 1 150); do
    curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"tool.completed"' && break
    sleep 0.2
done
curl -sf "$BASE/sources?session_id=$SID" | grep -q '"status":"ok"' \
    || fail "run 1 has no ok source row"
[ -f "spills/$SID/src-1.txt" ] || fail "run 1 did not persist the extracted text"

echo "== run 2: fixture down - the same url goes cache-first =="
kill "$TPID" 2>/dev/null
sleep 1
# Confirmed-down loop: the test relies on the fixture no longer answering.
# Windows node can ignore MSYS SIGTERM crossings (taskkill fallback there);
# on Linux a plain kill suffices, the loop still verifies the port.
fixture_down=0
for _ in $(seq 1 20); do
    case "$(uname -s)" in
        Linux*)
            if netstat -tln 2>/dev/null | grep ":$TLS_PORT " | grep -q LISTEN; then
                pid_listen=$(ss -tlnp 2>/dev/null | grep ":$TLS_PORT " | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2)
                [ -n "$pid_listen" ] && kill -9 "$pid_listen" 2>/dev/null
            fi
            ;;
        *)
            if netstat -ano 2>/dev/null | grep ":$TLS_PORT " | grep -q LISTEN; then
                pid_listen=$(netstat -ano 2>/dev/null | grep ":$TLS_PORT " | grep LISTEN | awk '{print $5}' | head -1)
                taskkill //F //PID "$pid_listen" > /dev/null 2>&1
            fi
            ;;
    esac
    if ! netstat -ano 2>/dev/null | grep ":$TLS_PORT " | grep -q LISTEN && \
       ! netstat -tln 2>/dev/null | grep ":$TLS_PORT " | grep -q LISTEN; then
        fixture_down=1
        break
    fi
    sleep 0.5
done
[ "$fixture_down" = "1" ] || fail "the fixture could not be taken down"
run_turn "fetch https://127.0.0.1:$TLS_PORT/ page two"
# Scope every run-2 assertion to run 2's events: the full session snapshot
# also contains run 1's run.completed/tool.completed, which would satisfy
# these greps instantly while run 2's fetch is still settling. Run 2's id
# is the last distinct run_id in the snapshot.
RUN2=""
SNAP=""
for _ in $(seq 1 100); do
    SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null)"
    if [ "$(printf '%s' "$SNAP" | grep -o '"run_id":"run-[0-9]*"' | sort -u | wc -l)" -ge 2 ]; then
        RUN2="$(printf '%s' "$SNAP" | grep -o '"run_id":"run-[0-9]*"' | tail -1 | cut -d'"' -f4)"
        break
    fi
    sleep 0.3
done
[ -n "$RUN2" ] || fail "run 2 never started"
# Poll for run 2's own run.completed (up to 90 s: tier-0 two attempts, then
# the escalation ladder) before judging the result.
for _ in $(seq 1 300); do
    TAIL="${SNAP#*\"run_id\":\"$RUN2\"}"
    printf '%s' "$TAIL" | grep -q '"event_type":"run.completed"' && break
    SNAP="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null)"
    sleep 0.3
done
TAIL="${SNAP#*\"run_id\":\"$RUN2\"}"
printf '%s' "$TAIL" | grep -q '"event_type":"tool.completed"' \
    || fail "run 2 had no tool call"
# The ok row must have survived the failure (not clobbered to error).
curl -sf "$BASE/sources?session_id=$SID" | grep -q '"status":"ok"' \
    || fail "the failure clobbered the ok source row"
printf '%s' "$TAIL" | grep -q "(cached from" \
    || fail "no cache marker in the run 2 fetch result"
printf '%s' "$TAIL" | grep -q 'nyaxus tls fixture' \
    || fail "the cached result does not decode the fixture page"

echo "WEB_CACHE_TEST_PASSED"
