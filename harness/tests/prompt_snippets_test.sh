#!/usr/bin/env bash
# End-to-end check for the prompt-snippets routes (docs/prompt-snippets.md):
# list / save / delete the per-message rule files in the workspace's
# .agents/prompt-snippets/ (frontmatter: placement prepend|append, order).
#
# Usage: harness/tests/prompt_snippets_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
DAEMON_PORT=8972
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

DAEMON_PID=""
cleanup() {
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
}
trap cleanup EXIT

echo "== health =="
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:9/v1" --model mock-model > daemon.log 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 75); do
    curl -sf "$BASE/health" > /dev/null && break
    sleep 0.2
done
kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon process died (port $DAEMON_PORT in use?) - see $WORKDIR/daemon.log"
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "ok"

echo "== empty workspace: no snippets =="
LIST="$(curl -sf "$BASE/prompt-snippets")"
[ "$LIST" = "[]" ] || fail "expected an empty list, got: $LIST"

echo "== save two snippets (prepend + append) =="
curl -sf -X POST "$BASE/prompt-snippets" -H 'Content-Type: application/json' \
    -d '{"name":"tone","placement":"prepend","order":10,"content":"Be concise."}' > /dev/null \
    || fail "save tone failed"
curl -sf -X POST "$BASE/prompt-snippets" -H 'Content-Type: application/json' \
    -d '{"name":"citations","placement":"append","order":0,"content":"Cite sources."}' > /dev/null \
    || fail "save citations failed"
LIST="$(curl -sf "$BASE/prompt-snippets")"
echo "$LIST" | grep -q '"name":"tone"' || fail "tone is not in the list: $LIST"
echo "$LIST" | grep -q '"name":"citations"' || fail "citations is not in the list: $LIST"
echo "$LIST" | grep -q '"placement":"prepend"' || fail "tone lost its placement: $LIST"
echo "$LIST" | grep -q '"content":"Be concise."' || fail "tone lost its content: $LIST"

echo "== re-save overwrites, no duplicate =="
curl -sf -X POST "$BASE/prompt-snippets" -H 'Content-Type: application/json' \
    -d '{"name":"tone","placement":"append","order":5,"content":"Be brief."}' > /dev/null \
    || fail "re-save failed"
COUNT="$(curl -sf "$BASE/prompt-snippets" | grep -o '"name":"tone"' | wc -l | tr -d ' ')"
[ "$COUNT" = "1" ] || fail "re-save duplicated tone (count $COUNT)"
curl -sf "$BASE/prompt-snippets" | grep -q '"content":"Be brief."' \
    || fail "re-save did not overwrite the content"

echo "== delete =="
curl -sf -X POST "$BASE/prompt-snippets/delete" -H 'Content-Type: application/json' \
    -d '{"name":"citations"}' > /dev/null || fail "delete failed"
LIST="$(curl -sf "$BASE/prompt-snippets")"
echo "$LIST" | grep -q '"name":"citations"' && fail "citations is still in the list: $LIST"
echo "$LIST" | grep -q '"name":"tone"' || fail "delete removed the wrong snippet: $LIST"

echo "== frontmatter is written and re-parsed =="
[ -f "$WORKDIR/.agents/prompt-snippets/tone.md" ] \
    || fail "the snippet file is not on disk"
grep -q 'placement: append' "$WORKDIR/.agents/prompt-snippets/tone.md" \
    || fail "the frontmatter placement is missing"
curl -sf "$BASE/prompt-snippets" | grep -q '"placement":"append"' \
    || fail "the re-parsed placement is wrong"

echo
echo "PROMPT_SNIPPETS_TEST_PASSED"
