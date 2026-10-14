#!/usr/bin/env bash
# End-to-end check for the native TLS fetch (vendored mbedtls, web.fetch tier
# 0): a local https fixture with its own self-signed CA is fetched by the
# daemon with `web.ca_bundle` pinned to that CA - the same call the model
# makes on the open internet, minus the public-network variable. Asserts:
#   - the tool result cites HTTP 200 with the fixture's title + readable text
#   - the source ledger recorded the fixture URL
#   - "fetched natively" provenance on the daemon log (tier 0, no Firecrawl)
#
# Usage: harness/tests/web_tls_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9081      # mock provider (the model)
TLS_PORT=9445       # https fixture (the fetch target)
DAEMON_PORT=9082
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
# The daemon is a native process: MSYS-style /tmp paths in its settings must
# be Windows paths (cygpath; empty pass-through where cygpath is absent).
DAEMON_WORKDIR="$WORKDIR"
if command -v cygpath > /dev/null; then
    DAEMON_WORKDIR="$(cygpath -m "$WORKDIR")"
fi

echo "== fixture: self-signed CA + one page =="
if command -v openssl > /dev/null; then
    MSYS2_ARG_CONV_EXCL="*" MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 \
        -keyout key.pem -out ca.pem -nodes -days 3650 \
        -subj "/CN=nyaxus-tls-fixture" \
        -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" > /dev/null 2>&1
fi
if [ ! -f ca.pem ]; then
    # No openssl on the box: fall back to the dev-tree fixture creds if present.
    cp "$ROOT/tls-f2/ca.pem" "$ROOT/tls-f2/key.pem" . 2>/dev/null
fi
[ -f ca.pem ] && [ -f key.pem ] || { echo "FAIL: no fixture keys" >&2; exit 1; }

fail() { echo "FAIL: $1" >&2; exit 1; }

CALLS='[{"name":"web.fetch","arguments":{"url":"https://localhost:9445/"}},{"text":"done"}]' \
    MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_spill_provider.mjs" > mock.log 2>&1 &
MPID=$!
"$NODE" "$ROOT/harness/tests/tls_fixture.mjs" "$TLS_PORT" \
    "$WORKDIR/ca.pem" "$WORKDIR/key.pem" > fixture.log 2>&1 &
TPID=$!
trap 'kill "$DPID" "$MPID" "$TPID" 2>/dev/null || true' EXIT
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo ok

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

# CA pinning proves the web.ca_bundle setting flows into the transport: the
# fixture's self-signed CA is the only trust root that validates it.
curl -sf -X POST "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"web.ca_bundle":"'"$DAEMON_WORKDIR"'/ca.pem"}' > /dev/null \
    || fail "the web.ca_bundle setting was rejected"

RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"fetch the fixture page\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "the run did not start"

DONE=""
for _ in $(seq 1 150); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "the run did not complete (see daemon.log)"

SNAP="$(curl -sf "$BASE/sessions/$SID")"
printf '%s' "$SNAP" | grep -q '"event_type":"tool.completed"' \
    || fail "no tool.completed under a completed run"
printf '%s' "$SNAP" | grep -q 'nyaxus tls fixture' \
    || fail "the fetched page is not the fixture's content"
if printf '%s' "$SNAP" | grep -q '"error":"'; then
    fail "a tool error surfaced under a successful fetch"
fi
curl -sf "$BASE/sources?session_id=$SID" | grep -q '"status":"ok"' \
    || fail "the source ledger has no ok row for the fetch"
grep -q "fetched natively" daemon.log \
    || fail "the fetch did not report native TLS provenance"

echo "WEB_TLS_TEST_PASSED"
