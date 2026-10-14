#!/usr/bin/env bash
# md_log_test.sh - the md-log vault mirror: linking a session to an existing
# markdown note backfills the session's turns as Obsidian callouts, live
# turns append as they happen, and unlinking stops the mirroring.
#
# Covers:
#   - the fs listing route (path picker): entries for a directory, 400 for
#     relative and non-directory paths
#   - linking a file that does not exist: 400, and the daemon never creates it
#   - linking an existing note backfills: user turns as [!quote], the quiz
#     question as [!question] *before* the graded [!success]/[!note] answer,
#     assistant prose verbatim
#   - a turn taken while linked appends live (question before answer)
#   - unlinking stops appends (file byte-identical across a turn)
#
# Usage: harness/tests/md_log_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9035
DAEMON_PORT=9036
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
# The daemon is a native Windows process: std::filesystem does not translate
# MSYS paths, so the vault lives at the Windows form of the temp dir.
WORKDIR_WIN="$(cygpath -m "$WORKDIR")"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_mdlog_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

VAULT="$WORKDIR_WIN/vault"
NOTE="$VAULT/note.md"
mkdir -p "$VAULT"
printf 'seed line\n' > "$NOTE"

echo "== fs route: directory listing for the path picker =="
CODE="$(curl -sG -o fs.json -w '%{http_code}' "$BASE/fs" --data-urlencode "path=$VAULT")"
[ "$CODE" = "200" ] || fail "fs listing of the vault dir returned $CODE"
grep -q '"name":"note.md"' fs.json || fail "the vault note is missing from the listing"
grep -q '"dir":false' fs.json || fail "the listing does not mark file entries"
CODE="$(curl -sG -o /dev/null -w '%{http_code}' "$BASE/fs" --data-urlencode "path=vault")"
[ "$CODE" = "400" ] || fail "a relative path was accepted by the fs route"
CODE="$(curl -sG -o /dev/null -w '%{http_code}' "$BASE/fs" --data-urlencode "path=$VAULT/note.md")"
[ "$CODE" = "400" ] || fail "a file path was accepted where a directory is required"
echo "fs listing ok"

# Extract one field of the first input.requested event of a given kind for a
# given run (earlier runs' events linger in the ledger). Args:
# session-json-file run kind field(input_id|options)
input_field() {
    "$NODE" -e '
        const fs = require("fs");
        const j = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
        const run = process.argv[2];
        const kind = process.argv[3];
        const field = process.argv[4];
        const ev = (j.events || []).find((e) =>
            e.event_type === "input.requested" && e.run_id === run &&
            e.payload && e.payload.kind === kind);
        if (!ev) { console.log(""); process.exit(0); }
        console.log(String(ev.payload[field] === undefined ? "" : ev.payload[field]));
    ' "$1" "$2" "$3" "$4"
}

# 1 when the given run has a run.completed event in the ledger.
run_completed() {
    "$NODE" -e '
        const fs = require("fs");
        const run = process.argv[1];
        const j = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
        const done = (j.events || []).some(
            (e) => e.event_type === "run.completed" && e.run_id === run);
        console.log(done ? "1" : "");
    ' "$1" "$2"
}

# Start a run, answer the quiz (correct option) and the follow-up question
# (first option), and wait for run.completed.
run_turn() {
    local message="$1"
    local run input_id choice options
    run="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$message\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start for: $message"
    for _ in $(seq 1 50); do
        curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
        input_id="$(input_field session.json "$run" quiz input_id)"
        if [ -n "$input_id" ]; then break; fi
        sleep 0.2
    done
    [ -n "$input_id" ] || fail "the quiz input.requested never appeared (see $WORKDIR/daemon.log)"
    options="$(input_field session.json "$run" quiz options)"
    choice="$("$NODE" -e '
        const options = JSON.parse(process.argv[1]);
        console.log(options.indexOf("Paris"));
    ' "$options")"
    [ "$choice" -ge 0 ] || fail "the correct option is missing from the presented options: $options"
    curl -sf -X POST "$BASE/runs/$run/commands" -H 'Content-Type: application/json' \
        -d "{\"command\":\"answer\",\"input_id\":\"$input_id\",\"choice\":\"$choice\"}" > /dev/null
    for _ in $(seq 1 50); do
        curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
        input_id="$(input_field session.json "$run" question input_id)"
        if [ -n "$input_id" ]; then break; fi
        sleep 0.2
    done
    [ -n "$input_id" ] || fail "the ask_user_question input never appeared (see $WORKDIR/daemon.log)"
    curl -sf -X POST "$BASE/runs/$run/commands" -H 'Content-Type: application/json' \
        -d "{\"command\":\"answer\",\"input_id\":\"$input_id\",\"choice\":\"0\"}" > /dev/null
    for _ in $(seq 1 75); do
        curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
        if [ "$(run_completed "$run" session.json)" = "1" ]; then
            return 0
        fi
        sleep 0.2
    done
    fail "the run did not complete for: $message (see $WORKDIR/daemon.log)"
}

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

echo "== link: the daemon refuses a file it does not own =="
CODE="$(curl -s -o link-missing.json -w '%{http_code}' -X POST "$BASE/sessions/$SID/md-log" \
    -H 'Content-Type: application/json' -d "{\"path\":\"$VAULT/missing.md\"}")"
[ "$CODE" = "400" ] || fail "linking a missing file returned $CODE, want 400: $(cat link-missing.json)"
[ ! -e "$VAULT/missing.md" ] || fail "the daemon created the vault file it should only append to"
echo "missing file rejected, not created"

echo "== turn one (unlinked), then link: the backfill renders the ledger =="
run_turn "quiz me"
LINK="$(curl -s -X POST "$BASE/sessions/$SID/md-log" -H 'Content-Type: application/json' \
    -d "{\"path\":\"$NOTE\"}")"
printf '%s' "$LINK" | grep -q '"ok":true' || fail "the link route did not accept the note: $LINK"
echo "note linked"

grep -q "seed line" "$NOTE" || fail "the backfill clobbered the note's existing content"
grep -qF '> [!quote]' "$NOTE" || fail "the user turn is missing its [!quote] callout"
grep -q "quiz me" "$NOTE" || fail "the user text is missing from the note"
grep -qF '> [!question]' "$NOTE" || fail "the quiz question is missing its [!question] callout"
grep -q "What is the capital of France?" "$NOTE" || fail "the quiz question text is missing from the note"
grep -qF '> [!success]' "$NOTE" || fail "the correct quiz answer is missing its [!success] callout"
grep -q "Correct. The user chose: Paris." "$NOTE" || fail "the quiz grade is missing from the note"
grep -qF '> [!note]' "$NOTE" || fail "the ask_user_question answer is missing its [!note] callout"
grep -q "first turn done" "$NOTE" || fail "the assistant prose is missing from the note"
Q_LINE="$(grep -nF '> [!question]' "$NOTE" | head -1 | cut -d: -f1)"
S_LINE="$(grep -nF '> [!success]' "$NOTE" | head -1 | cut -d: -f1)"
N_LINE="$(grep -nF '> [!note]' "$NOTE" | head -1 | cut -d: -f1)"
[ -n "$Q_LINE" ] && [ -n "$S_LINE" ] && [ -n "$N_LINE" ] \
    || fail "could not locate the callout lines in the note"
[ "$Q_LINE" -lt "$S_LINE" ] || fail "the question (line $Q_LINE) must precede its answer (line $S_LINE)"
[ "$S_LINE" -lt "$N_LINE" ] || fail "the quiz answer (line $S_LINE) must precede the ask answer (line $N_LINE)"
echo "backfill order correct"

echo "== turn two (linked): the live append keeps question before answer =="
run_turn "quiz me again"
grep -q "quiz me again" "$NOTE" || fail "the live user turn did not append to the note"
grep -q "second turn live" "$NOTE" || fail "the live assistant prose did not append to the note"
QUOTE2_LINE="$(grep -nF '> [!quote]' "$NOTE" | tail -1 | cut -d: -f1)"
S2_LINE="$(grep -nF '> [!success]' "$NOTE" | tail -1 | cut -d: -f1)"
# The question that belongs to the live success: the last [!question] above it
# (the later ask question trails it and would skew a naive last-match).
Q2_LINE="$(grep -nF '> [!question]' "$NOTE" | awk -F: -v s="$S2_LINE" '$1+0 < s+0 { q = $1 } END { print q }')"
[ -n "$Q2_LINE" ] || fail "no [!question] precedes the live [!success]"
[ "$Q2_LINE" -gt "$N_LINE" ] || fail "the live question (line $Q2_LINE) did not append after the backfill (line $N_LINE)"
[ "$Q2_LINE" -lt "$S2_LINE" ] || fail "the live question (line $Q2_LINE) must precede its answer (line $S2_LINE)"
echo "live append order correct"

echo "== unlink: turns stop mirroring =="
UNLINK="$(curl -s -X POST "$BASE/sessions/$SID/md-log" -H 'Content-Type: application/json' -d '{"path":""}')"
printf '%s' "$UNLINK" | grep -q '"ok":true' || fail "the unlink route did not accept: $UNLINK"
curl -sf "$BASE/sessions/$SID" > session.json
grep -q '"md_log_path":""' session.json || fail "the session still carries the vault path after unlink"
cp "$NOTE" before-unlink.md
run_turn "after unlink"
cmp -s "$NOTE" before-unlink.md || fail "the note changed after unlinking"
echo "no appends after unlink"

echo
echo "MD_LOG_TEST_PASSED"
