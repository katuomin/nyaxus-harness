#!/usr/bin/env bash
# tags_test.sh - session tags: the label round-trip and its validation.
#
# Asserts:
#   - a fresh session serializes tags as an empty JSON array
#   - POST /sessions/{id}/tags persists labels, visible on GET session and
#     the session list (JSON array of strings, input order preserved)
#   - labels are trimmed and deduped on save
#   - an empty array clears the tags
#   - validation: non-array body, non-string element, empty label, >32-char
#     label, and >12 labels all 400; an unknown session 404s
#
# Usage: harness/tests/tags_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
DAEMON_PORT=9043
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

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

echo "== fresh session: tags serialize as an empty array =="
curl -sf "$BASE/sessions/$SID" | grep -q '"tags":\[\]' || fail "fresh session must serialize tags as []"

echo "== set tags =="
STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/tags" \
    -H 'Content-Type: application/json' -d '{"tags":["ops","urgent"]}')"
[ "$STATUS" = "200" ] || fail "set tags returned $STATUS, want 200"
curl -sf -X POST "$BASE/sessions/$SID/tags" \
    -H 'Content-Type: application/json' -d '{"tags":["ops","urgent"]}' \
    | grep -q '"tags":\["ops","urgent"\]' || fail "response must echo the saved tags"
curl -sf "$BASE/sessions/$SID" | grep -q '"tags":\["ops","urgent"\]' || fail "GET session must show the tags"
curl -sf "$BASE/sessions" | grep -q '"tags":\["ops","urgent"\]' || fail "session list must show the tags"

echo "== trim + dedupe on save =="
curl -sf -X POST "$BASE/sessions/$SID/tags" \
    -H 'Content-Type: application/json' -d '{"tags":["ops"," ops ", "URGENT"]}' \
    | grep -q '"tags":\["ops","URGENT"\]' || fail "labels must be trimmed and deduped"

echo "== empty array clears the tags =="
curl -sf -X POST "$BASE/sessions/$SID/tags" \
    -H 'Content-Type: application/json' -d '{"tags":[]}' \
    | grep -q '"tags":\[\]' || fail "an empty array must clear the tags"
curl -sf "$BASE/sessions/$SID" | grep -q '"tags":\[\]' || fail "GET session must show cleared tags"

echo "== validation =="
LONG_TAG="$(printf 'a%.0s' {1..33})"
for BODY in '{}' '{"tags":"ops"}' '{"tags":["ops",42]}' '{"tags":[""]}' "{\"tags\":[\"$LONG_TAG\"]}" \
            '{"tags":["1","2","3","4","5","6","7","8","9","10","11","12","13"]}'; do
    STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/tags" \
        -H 'Content-Type: application/json' -d "$BODY")"
    [ "$STATUS" = "400" ] || fail "body $BODY returned $STATUS, want 400"
done

STATUS="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/does-not-exist/tags" \
    -H 'Content-Type: application/json' -d '{"tags":["ops"]}')"
[ "$STATUS" = "404" ] || fail "unknown session returned $STATUS, want 404"

echo
echo "TAGS_TEST_PASSED"
