#!/usr/bin/env bash
# chat_title_test.sh - chat.title guardrails: lifecycle, hard guard, harvest.
#
# Covers:
#   - the tool is declared while the title is provisional (first few turns)
#     and drops from the manifest the moment the model sets a title
#   - a chat.title call finalizes the title and harvests the (message, title)
#     pair for training data
#   - manual re-title (POST /sessions/{id}/retitle) re-arms the tool for the
#     next run
#   - hard guard: a stray chat.title call when the title is final and no
#     re-title is pending is a no-op (title unchanged, no new harvest row)
#
# Usage: harness/tests/chat_title_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8950
DAEMON_PORT=8951
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
MOCK="http://127.0.0.1:$MOCK_PORT"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
# The daemon stores nyaxus.db (and the title harvest) in its CWD.
mkdir -p da

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_chat_title_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd da && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

wait_health() {
    for _ in $(seq 1 50); do
        if curl -sf "$BASE/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

# Wait until the run's run.completed event shows up in the ledger.
wait_completed() {
    local sid="$1"
    for _ in $(seq 1 100); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

session_title() {
    curl -sf "$BASE/sessions/$1" | sed -n 's/.*"title":"\([^"]*\)".*/\1/p' | head -1
}

# Does recorded request N (1-based) declare the named tool?
request_has_tool() {
    local n="$1" tool="$2"
    curl -sf "$MOCK/requests" | "$NODE" -e "
        let d = '';
        process.stdin.on('data', (c) => { d += c; });
        process.stdin.on('end', () => {
            const reqs = JSON.parse(d);
            const req = reqs[Number(process.argv[1]) - 1];
            const names = ((req && req.tools) || []).map((t) => t.function && t.function.name);
            process.exit(names.includes(process.argv[2]) ? 0 : 1);
        });" "$n" "$tool"
}

harvest_rows() {
    wc -l < "$WORKDIR/da/title-harvest/chat-title-harvest.jsonl" 2>/dev/null | tr -d ' '
}

echo "== health =="
wait_health || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

# 120+ chars: not a thin opening, so the title is the truncated first line
# (no title model configured) and the title is provisional ("auto").
MSG="please investigate the daemon log for the recurring provider timeout error and summarize what you find"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

echo "== run 1: provisional title, tool declared, model sets it =="
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"$MSG\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run 1 did not start"
wait_completed "$SID" || fail "run 1 did not complete"

TITLE1="$(session_title "$SID")"
[ "$TITLE1" = "Mock Title Check" ] || fail "title not set by chat.title (got '$TITLE1')"
echo "title set by the model"

request_has_tool 1 "chat.title" || fail "request 1 did not declare chat.title (provisional title, turn 1)"
request_has_tool 2 "chat.title" && fail "request 2 still declares chat.title after the title is final"
echo "tool declared while provisional, dropped after the title is final"

ROWS1="$(harvest_rows)"
[ "$ROWS1" = "1" ] || fail "harvest rows after run 1: expected 1, got $ROWS1"
grep -q "Mock Title Check" "$WORKDIR/da/title-harvest/chat-title-harvest.jsonl" || fail "harvest row missing the model title"
grep -q "recurring provider timeout" "$WORKDIR/da/title-harvest/chat-title-harvest.jsonl" || fail "harvest row missing the user message"
echo "pair harvested (user message + model title)"

echo "== run 2: manual re-title re-arms the tool =="
CODE="$(curl -s -o /dev/null -w "%{http_code}" -X POST "$BASE/sessions/$SID/retitle")"
[ "$CODE" = "200" ] || fail "retitle endpoint returned $CODE"
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"keep going, that looks right\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "run 2 did not start"
wait_completed "$SID" || fail "run 2 did not complete"

TITLE2="$(session_title "$SID")"
[ "$TITLE2" = "Retitled Mock" ] || fail "re-title not applied (got '$TITLE2')"
echo "manual re-title applied"

request_has_tool 3 "chat.title" || fail "request 3 did not declare chat.title (re-title pending)"
request_has_tool 4 "chat.title" && fail "request 4 still declares chat.title after the re-title is consumed"
ROWS2="$(harvest_rows)"
[ "$ROWS2" = "2" ] || fail "harvest rows after run 2: expected 2, got $ROWS2"
echo "re-armed for the re-title run, dropped after, second pair harvested"

echo "== run 3: hard guard on a stray call =="
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"and one more check, thanks\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "run 3 did not start"
wait_completed "$SID" || fail "run 3 did not complete"

TITLE3="$(session_title "$SID")"
[ "$TITLE3" = "Retitled Mock" ] || fail "stray chat.title changed the final title (got '$TITLE3')"
request_has_tool 5 "chat.title" && fail "request 5 declares chat.title for a final title with no pending re-title"
curl -sf "$BASE/sessions/$SID" | grep -q "Title already set" || fail "hard guard no-op message missing from the ledger"
ROWS3="$(harvest_rows)"
[ "$ROWS3" = "2" ] || fail "harvest rows after run 3: expected 2, got $ROWS3 (no-op must not harvest)"
echo "stray call no-oped: title unchanged, tool undeclared, no harvest row"

echo
echo "CHAT_TITLE_TEST_PASSED"
