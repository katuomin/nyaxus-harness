#!/usr/bin/env bash
# End-to-end check for the skills trash/restore + enable toggle, and the
# <skill_content> activation envelope.
#
#   A. save skill "alpha" -> listed with enabled=true
#   B. disable "alpha" -> enabled=false, and the state survives a daemon
#      restart (the toggle lives in the store, not the process)
#   C. delete "alpha" -> it is trashed, not removed: gone from /skills,
#      present in /skills/trash, the directory moved to skills-trash/
#   D. restore "alpha" -> back in /skills, trash empty
#   E. delete + trash/delete -> permanently gone
#   F. a run whose provider answers with a "skill" tool call: the result is
#      the <skill_content> envelope (source + directory + escaped body)
#
# Usage: harness/tests/skills_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8971
DAEMON_PORT=8972
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# The global-skill phase (F) needs a skill in the global root, which is
# host state (a prior uninstall removed it here) - supply a fake home the
# daemon reads HOME/USERPROFILE from, with the skill inside.
mkdir -p home/.agents/skills/grilling
cat > home/.agents/skills/grilling/SKILL.md <<'SKILL'
---
name: grilling
description: Stress-test a plan with one hard question at a time
---

# Grilling

Interrogate the plan until the weakest assumption is visible.
SKILL
DAEMON_HOME="$WORKDIR/home"
if command -v cygpath > /dev/null; then
    DAEMON_HOME="$(cygpath -m "$DAEMON_HOME")"
fi

MOCK_PID=""
DAEMON_PID=""
cleanup() {
    [ -n "$MOCK_PID" ] && kill "$MOCK_PID" 2>/dev/null || true
    [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
}
trap cleanup EXIT

start_daemon() {
    USERPROFILE="$DAEMON_HOME" HOME="$DAEMON_HOME" \
        "$DAEMON" --port "$DAEMON_PORT" --provider openai \
        --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
    DAEMON_PID=$!
    for _ in $(seq 1 75); do
        curl -sf "$BASE/health" > /dev/null && break
        sleep 0.2
    done
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon process died (port $DAEMON_PORT in use?) - see $WORKDIR/daemon.log"
}

# skill "alpha" from the /skills list: its JSON object, empty when absent
skill_alpha() {
    curl -sf "$BASE/skills" 2>/dev/null | tr ',' '\n' | tr '\n' ' ' \
        | grep -o '{[^{}]*"name":"alpha"[^{}]*}' | head -1
}

start_mock() {
    MOCK_PORT="$MOCK_PORT" TOOLS="$1" "$NODE" "$ROOT/harness/tests/mock_tasks_loop.mjs" > mock.log 2>&1 &
    MOCK_PID=$!
    for _ in $(seq 1 25); do
        kill -0 "$MOCK_PID" 2>/dev/null || break
        sleep 0.2
    done
    kill -0 "$MOCK_PID" 2>/dev/null || fail "mock process died (port $MOCK_PORT in use?) - see $WORKDIR/mock.log"
}

session_ledger() {
    curl -sf "$BASE/sessions/$SID" 2>/dev/null
}

wait_run_done() {
    for _ in $(seq 1 350); do
        if curl -sf "$BASE/sessions/$SID" 2>/dev/null \
            | grep -q '"event_type":"run.completed"'; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
start_daemon
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "ok"

# ── Phase A: save alpha, listed and enabled ───────────────────────────────
echo "== phase A: save skill alpha =="
curl -sf -X POST "$BASE/skills" -H 'Content-Type: application/json' \
    -d '{"name":"alpha","description":"test skill","content":"do the thing"}' > /dev/null \
    || fail "save skill failed"
ALPHA="$(skill_alpha)"
[ -n "$ALPHA" ] || fail "alpha is not in the /skills list"
echo "$ALPHA" | grep -q '"enabled":true' \
    || fail "alpha is not enabled by default: $ALPHA"
echo "ok"

# ── Phase B: disable, and the state survives a restart ────────────────────
echo "== phase B: disable alpha, state survives restart =="
curl -sf -X POST "$BASE/skills/enable" -H 'Content-Type: application/json' \
    -d '{"name":"alpha","enabled":false}' > /dev/null \
    || fail "disable failed"
ALPHA="$(skill_alpha)"
echo "$ALPHA" | grep -q '"enabled":false' \
    || fail "alpha is not disabled after the toggle: $ALPHA"
kill "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=""
sleep 0.5
start_daemon
ALPHA="$(skill_alpha)"
[ -n "$ALPHA" ] || fail "alpha vanished after the restart"
echo "$ALPHA" | grep -q '"enabled":false' \
    || fail "the disabled state did not survive the restart: $ALPHA"
# Re-enable so the later phases are about trash, not visibility.
curl -sf -X POST "$BASE/skills/enable" -H 'Content-Type: application/json' \
    -d '{"name":"alpha","enabled":true}' > /dev/null || fail "re-enable failed"
echo "ok"

# ── Phase C: delete = trash ───────────────────────────────────────────────
echo "== phase C: delete alpha -> trashed, not removed =="
curl -sf -X POST "$BASE/skills/delete" -H 'Content-Type: application/json' \
    -d '{"name":"alpha"}' > /dev/null || fail "delete failed"
[ -z "$(skill_alpha)" ] || fail "alpha is still in /skills after delete"
TRASH="$(curl -sf -X POST "$BASE/skills/trash" -H 'Content-Type: application/json')"
echo "$TRASH" | grep -q '"name":"alpha"' \
    || fail "alpha is not in /skills/trash: $TRASH"
[ -f "$WORKDIR/.agents/skills-trash/alpha/SKILL.md" ] \
    || fail "the skill directory was not moved to skills-trash/"
[ ! -d "$WORKDIR/.agents/skills/alpha" ] \
    || fail "the skill directory is still in the skills root"
echo "ok"

# ── Phase D: restore ──────────────────────────────────────────────────────
echo "== phase D: restore alpha =="
curl -sf -X POST "$BASE/skills/restore" -H 'Content-Type: application/json' \
    -d '{"name":"alpha"}' > /dev/null || fail "restore failed"
ALPHA="$(skill_alpha)"
[ -n "$ALPHA" ] || fail "alpha is not back in /skills after restore"
TRASH="$(curl -sf -X POST "$BASE/skills/trash" -H 'Content-Type: application/json')"
echo "$TRASH" | grep -q '"name":"alpha"' \
    && fail "alpha is still in the trash after restore"
[ -f "$WORKDIR/.agents/skills/alpha/SKILL.md" ] \
    || fail "the skill directory did not come back to the skills root"
echo "ok"

# ── Phase E: permanent delete from the trash ──────────────────────────────
echo "== phase E: delete from trash, permanently gone =="
curl -sf -X POST "$BASE/skills/delete" -H 'Content-Type: application/json' \
    -d '{"name":"alpha"}' > /dev/null || fail "second delete failed"
curl -sf -X POST "$BASE/skills/trash/delete" -H 'Content-Type: application/json' \
    -d '{"name":"alpha"}' > /dev/null || fail "trash delete failed"
TRASH="$(curl -sf -X POST "$BASE/skills/trash" -H 'Content-Type: application/json')"
echo "$TRASH" | grep -q '"name":"alpha"' \
    && fail "alpha is still in the trash after permanent delete"
[ ! -d "$WORKDIR/.agents/skills-trash/alpha" ] \
    || fail "the trashed directory is still on disk"
echo "ok"

# ── Phase F: the skill tool renders the <skill_content> envelope ──────────
echo "== phase F: skill tool result is the <skill_content> envelope =="
start_mock '[{"name":"skill","arguments":{"name":"grilling"}}]'
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"run it\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"
wait_run_done || fail "run did not complete"
LEDGER="$(session_ledger)"
echo "$LEDGER" | grep -q '"event_type":"tool.completed"' \
    || fail "no tool.completed for the skill call"
echo "$LEDGER" | grep -q 'skill_content name=\\"grilling\\"' \
    || fail "the skill result is not the <skill_content> envelope"
echo "$LEDGER" | grep -q '<source>global</source>' \
    || fail "the envelope is missing the source label"
echo "$LEDGER" | grep -q 'resolve against <directory>' \
    || fail "the envelope is missing the relative-path note"
echo "$LEDGER" | grep -q '<\/skill_content>' \
    || fail "the envelope is missing its closing tag"
echo "ok"

echo
echo "SKILLS_TEST_PASSED"
