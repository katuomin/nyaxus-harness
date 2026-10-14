#!/usr/bin/env bash
# crash_handler_test.sh - the fatal-signal breadcrumb works.
#
# Asserts:
#   - the daemon launched with --selftest-crash comes up healthy, then
#     abort()s (SIGABRT) ~750 ms after the listen log
#   - its stderr carries "[nyaxus] FATAL: SIGABRT" followed by stack frames
#     (the prod crash-breadcrumb path: journald gets the crashing thread's
#     stack even with ulimit -c = 0)
#
# Usage: harness/tests/crash_handler_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
DAEMON_PORT=9041
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

echo "== launch with --selftest-crash =="
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:19999/v1" --model mock-model \
    --selftest-crash > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" 2>/dev/null || true' EXIT

# The daemon must answer health before it crashes (the flag aborts ~750 ms
# after the listen log; a health 200 from a dead listener proves nothing, so
# the pid liveness check below is part of the assertion too).
for _ in $(seq 1 50); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null 2>&1 || fail "daemon did not come up (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "healthy before the selftest abort"

# Wait for the abort to land.
for _ in $(seq 1 100); do
    kill -0 "$DPID" 2>/dev/null || break
    sleep 0.2
done
kill -0 "$DPID" 2>/dev/null && fail "daemon did not exit after the selftest abort"
echo "aborted"

grep -q "\[nyaxus\] FATAL: SIGABRT" daemon.log \
    || fail "FATAL breadcrumb missing from daemon.log"
# Frames: at least one hex address line after the header (backtrace_symbols_fd
# on POSIX, raw 0x addresses on Windows).
awk '/\[nyaxus\] FATAL: SIGABRT/{found=1; next}
     found && /0x[0-9a-f]+/{ok=1; exit}
     END{exit ok ? 0 : 1}' daemon.log \
    || fail "no stack frames after the FATAL header (see $WORKDIR/daemon.log)"
echo "breadcrumb + frames ok"

echo
echo "CRASH_HANDLER_TEST_PASSED"
