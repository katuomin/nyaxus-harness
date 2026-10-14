#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# start.sh - launch the nyaxus daemon and the web dev server together.
#
# Usage: ./start.sh
#        (runs from any folder; paths resolve relative to this file)
#
# The daemon runs in the background (logs: nyaxus-daemon.log,
# nyaxus-daemon-err.log) and is stopped automatically when the web
# server exits (Ctrl+C in this window).
# ---------------------------------------------------------------------------
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"

DAEMON=""
for candidate in \
    "$ROOT/harness/nyaxus_daemon" \
    "$ROOT/harness/nyaxus_daemon_linux" \
    "$ROOT/harness/nyaxus_daemon.exe"; do
    if [ -x "$candidate" ]; then DAEMON="$candidate"; break; fi
done
if [ -z "$DAEMON" ]; then
    echo "daemon binary not found in $ROOT/harness" >&2
    echo "build it first (see harness/README.md)" >&2
    exit 1
fi

PORT="${NYAXUS_PORT:-8787}"

echo "== daemon =="
echo "$DAEMON"
cd "$ROOT"
"$DAEMON" > "$ROOT/nyaxus-daemon.log" 2> "$ROOT/nyaxus-daemon-err.log" &
DPID=$!
trap 'kill "$DPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 30); do
    if curl -sf "http://127.0.0.1:$PORT/api/v1/health" > /dev/null 2>&1; then
        break
    fi
    sleep 1
done
if ! curl -sf "http://127.0.0.1:$PORT/api/v1/health" > /dev/null 2>&1; then
    echo "daemon did not become healthy, see $ROOT/nyaxus-daemon-err.log" >&2
    exit 1
fi
echo "daemon healthy on :$PORT"

echo "== web =="
# Prod is the daily driver (the dev React build allocates several times more
# per render and balloons the renderer over long sessions); --dev restores
# the dev server for development.
cd "$ROOT/web"
VITE_HARNESS_MODE=http
export VITE_HARNESS_MODE
if [ "${1:-}" = "--dev" ]; then
    echo "mode: dev"
    npm run dev
else
    echo "mode: prod (build + preview)"
    npm run build && npm run preview
fi
echo "(web server exited)"
