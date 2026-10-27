#!/usr/bin/env bash
# camofox_sticky_test.sh - the sticky camofox session e2e (web.camofox_sticky):
# the tier-2 fetch keeps one live browser per session instead of
# open/snapshot/close per call.
#
# Covers:
#   - sticky on: a fetch OPENS a tab and never deletes it at the end (the
#     tab, and its browser profile - cookies, logins - stay alive)
#   - a per-session userId ("nyaxus-session-<sid>"): the mock never sees the
#     shared ephemeral user for a sticky fetch
#   - a second fetch NAVIGATES the same tab (open count stays 1) and reads
#     the navigated page (the snapshot embeds the tab's current url)
#   - two web.fetch calls in one wave: the busy tab is not reused; the
#     second fetch opens its own (one tab per in-flight FETCH)
#   - the idle reaper: with web.camofox_sticky_ttl=0, the next fetch closes
#     every idle tab and opens fresh
#   - session delete closes the session's live tabs
#
# Usage: harness/tests/camofox_sticky_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
CAMOFOX_PORT=9102
PROVIDER_PORT=9103
DAEMON_PORT=9104
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
CAMOFOX="http://127.0.0.1:$CAMOFOX_PORT"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$CAMOFOX_PORT" "$NODE" "$ROOT/harness/tests/mock_camofox_sticky_server.mjs" > camofox.log 2>&1 &
CAMOPID=$!
MOCK_PORT="$PROVIDER_PORT" CAMOFOX_PORT="$CAMOFOX_PORT" \
    "$NODE" "$ROOT/harness/tests/mock_sticky_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_WEB_CAMOFOX_ENDPOINT="$CAMOFOX" NYAXUS_WEB_CAMOFOX_STICKY=1 \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$CAMOPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "healthy"

stats() { curl -sf "$CAMOFOX/stats"; }

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

run_fetch() {
    curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$1\"}" > /dev/null
    for _ in $(seq 1 100); do
        if [ "$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -o '"event_type":"run.completed"' | wc -l | tr -d ' ')" -ge "$2" ]; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== fetch one: sticky opens and keeps the tab =="
run_fetch "fetch page a" 1 || fail "run 1 did not complete"
S="$(stats)"
[ "$(echo "$S" | grep -o '"opened":[0-9]*' | grep -o '[0-9]*')" = "1" ] || fail "run 1 must open exactly one tab: $S"
[ "$(echo "$S" | grep -o '"deleted":[0-9]*' | grep -o '[0-9]*')" = "0" ] || fail "sticky must not delete the tab after the fetch: $S"
curl -sf "$BASE/sessions/$SID" | grep -q '\[src:1\]' || fail "run 1 produced no source id"
echo "tab opened and kept"

echo "== fetch two: the same tab is navigated =="
run_fetch "fetch page b" 2 || fail "run 2 did not complete"
S="$(stats)"
[ "$(echo "$S" | grep -o '"opened":[0-9]*' | grep -o '[0-9]*')" = "1" ] || fail "run 2 must navigate, not open: $S"
[ "$(echo "$S" | grep -o '"navigated":[0-9]*' | grep -o '[0-9]*')" = "1" ] || fail "run 2 must navigate once: $S"
curl -sf "$BASE/sessions/$SID" -o sticky_session.json \
    || fail "run 2 session fetch failed"
grep -qF "Sticky rendered the page at https://127.0.0.1:$CAMOFOX_PORT/page-b" sticky_session.json \
    || fail "run 2 must read the navigated page-b snapshot"
echo "navigated in place"

echo "== a wave of two fetches: one tab per in-flight FETCH =="
run_fetch "fetch page c and page d" 3 || fail "run 3 did not complete"
S="$(stats)"
[ "$(echo "$S" | grep -o '"opened":[0-9]*' | grep -o '[0-9]*')" = "2" ] || fail "the wave's second fetch must open its own tab: $S"
[ "$(echo "$S" | grep -o '"navigated":[0-9]*' | grep -o '[0-9]*')" = "2" ] || fail "the wave's first fetch must navigate once: $S"
[ "$(echo "$S" | grep -o '"deleted":[0-9]*' | grep -o '[0-9]*')" = "0" ] || fail "no tab may be deleted while sticky keeps them: $S"
echo "wave-safe: one busy tab left alone, one fresh tab opened"

echo "== the per-session userId, and no shared ephemeral user =="
echo "$S" | grep -q "\"users\":\[\"nyaxus-session-$SID\"\]" \
    || fail "sticky fetches must use exactly the per-session userId: $S"
echo "userId scoped per session"

echo "== the idle reaper: ttl=0 closes idle tabs at the next entry =="
curl -sf -X PUT "$BASE/settings" -H 'Content-Type: application/json' \
    -d '{"web.camofox_sticky_ttl":0}' > /dev/null || fail "ttl setting failed"
run_fetch "fetch page e" 4 || fail "run 4 did not complete"
S="$(stats)"
[ "$(echo "$S" | grep -o '"deleted":[0-9]*' | grep -o '[0-9]*')" = "2" ] || fail "the reaper must close the two idle tabs: $S"
[ "$(echo "$S" | grep -o '"opened":[0-9]*' | grep -o '[0-9]*')" = "3" ] || fail "after the reaper the fetch must open fresh: $S"
echo "reaped and reopened"

echo "== session delete closes the live tabs =="
curl -sf -X POST "$BASE/sessions/$SID/delete" > /dev/null || fail "session delete failed"
S="$(stats)"
[ "$(echo "$S" | grep -o '"deleted":[0-9]*' | grep -o '[0-9]*')" = "3" ] || fail "the session delete must close its live tab: $S"
[ "$(echo "$S" | grep -o '"liveTabs":\[\]')" ] || fail "the mock must hold no live tabs after the delete: $S"
echo "tabs closed with the session"

echo
echo "CAMOFOX_STICKY_TEST_PASSED"
