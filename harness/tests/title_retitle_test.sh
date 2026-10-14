#!/usr/bin/env bash
# title_retitle_test.sh - thin openings keep a faithful title, then retitle.
#
# Covers:
#   - a thin first message (< 100 chars) keeps the truncated first line as
#     its title (the title model confabulates on bare greetings - measured:
#     "Apple Watch Test" from "can you test something for me?")
#   - after the first completed run, the session is retitled from the
#     opening exchange (user + assistant), so the title reflects the actual
#     topic instead of the greeting
#
# Usage: harness/tests/title_retitle_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8970
DAEMON_PORT=8971
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
TITLE_MODEL="$ROOT/harness/models/nyx/qwen3-title-0.6b-int8.nyx"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$TITLE_MODEL" ] || { echo "title model not found: $TITLE_MODEL" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
mkdir -p da

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_title_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd da && NYAXUS_TITLE_MODEL="$TITLE_MODEL" exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

title_of() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            process.stdout.write(String(JSON.parse(d).title || ""));
        });'
}

echo "== health =="
for _ in $(seq 1 50); do
    if curl -sf "$BASE/health" > /dev/null 2>&1; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"hi, what can you do?\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"

echo "== thin opening keeps the faithful title =="
TITLE1="$(title_of "$SID")"
[ "$TITLE1" = "hi, what can you do?" ] || fail "thin opening did not keep the faithful title: $TITLE1"
echo "faithful provisional title: $TITLE1"

DONE=""
for _ in $(seq 1 150); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.failed"'; then
        fail "run failed: $(curl -sf "$BASE/sessions/$SID" | grep -o '"error":"[^"]*"' | head -1)"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (see $WORKDIR/daemon.log)"

echo "== retitled from the opening exchange =="
TITLE2="$(title_of "$SID")"
[ -n "$TITLE2" ] || fail "no title after the retitle"
[ "$TITLE2" != "$TITLE1" ] || fail "title was not retitled from the opening exchange: $TITLE2"
echo "retitled: $TITLE2"

echo
echo "TITLE_RETITLE_TEST_PASSED"
