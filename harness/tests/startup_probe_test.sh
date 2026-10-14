#!/usr/bin/env bash
# startup_probe_test.sh - a down provider must not block daemon startup.
#
# The provider endpoint is pointed at a non-routable address, so the
# startup model/context probe (15s client timeout) blocks in the
# background. The HTTP server and /health must be live well before that
# timeout, and the daemon must stay healthy after the probe gives up.
#
# Usage: harness/tests/startup_probe_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
# 8951 is profile_revision_test's mock port; a surviving mock there would
# steal the daemon's connections (Windows SO_REUSEADDR), so use a free port.
DAEMON_PORT=8953
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1"; kill "$DAEMON_PID" 2>/dev/null; exit 1; }

# 10.255.255.1 is non-routable: the SYN gets no RST, so the probe's HTTP
# client blocks until its 15s timeout. If startup discovery were
# synchronous, /health could not be live within 5s.
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://10.255.255.1:9/v1" --model mock-model > daemon.log 2>&1 &
DAEMON_PID=$!

echo "== health while the provider is blackholed =="
# NOTE: do not use `_` as the loop variable here - bash reassigns it to the
# last argument of the previous command (curl's URL), which would break the
# iteration check below.
for i in $(seq 1 25); do
    if curl -sf "$BASE/health" > /dev/null 2>&1; then
        echo "ok (daemon live, probe still running in the background)"
        break
    fi
    if [ "$i" = "25" ]; then
        cat daemon.log
        fail "daemon did not serve /health within 5s with an unreachable provider"
    fi
    sleep 0.2
done

echo "== probe gives up and logs a warning =="
for i in $(seq 1 50); do
    if grep -q "could not reach" daemon.log 2>/dev/null; then
        break
    fi
    sleep 0.5
done
grep -q "could not reach" daemon.log || { cat daemon.log; fail "expected a logged probe failure"; }
echo "ok"

echo "== daemon still healthy after the probe failure =="
curl -sf "$BASE/health" > /dev/null 2>&1 || fail "daemon unhealthy after the probe failure"
echo "ok"

kill "$DAEMON_PID" 2>/dev/null
echo
echo "STARTUP_PROBE_TEST_PASSED"
