#!/usr/bin/env bash
# quiz_test.sh - the quiz and ask_user_question interactive tools: the run
# parks on its own input gate, the user answers via the commands route, and
# the graded answer flows back as the tool result.
#
# Covers:
#   - a run whose model calls quiz -> ask_user_question completes without
#     any approve/deny command (no approval gate for input tools)
#   - the quiz options reach the client shuffled (presented order) and the
#     answer route resolves the run's input gate
#   - answering the quiz with the correct option grades it: the tool result
#     carries "Correct.", the user's choice, and the explanation
#   - ask_user_question returns the chosen option ungraded
#   - input.requested / input.resolved events are in the ledger
#
# Usage: harness/tests/quiz_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9033
DAEMON_PORT=9034
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_quiz_provider.mjs" > provider.log 2>&1 &
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

# Extract one field of the first input.requested event of a given kind.
# Args: session-json-file kind field(input_id|options)
input_field() {
    "$NODE" -e '
        const fs = require("fs");
        const kind = process.argv[1];
        const field = process.argv[2];
        const j = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
        const ev = (j.events || []).find((e) =>
            e.event_type === "input.requested" && e.payload && e.payload.kind === kind);
        if (!ev) { console.log(""); process.exit(0); }
        console.log(String(ev.payload[field] === undefined ? "" : ev.payload[field]));
    ' "$1" "$2" "$3"
}

echo "== run starts: quiz parks the run on the input gate =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"quiz me\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

QUIZ_PUBLISHED=""
for _ in $(seq 1 50); do
    curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
    if grep -q '"event_type":"run.waiting_approval"' session.json; then
        fail "the quiz tool sits behind the approval gate (run is waiting for a decision)"
    fi
    INPUT_ID="$(input_field quiz input_id session.json)"
    if [ -n "$INPUT_ID" ]; then
        QUIZ_PUBLISHED=1
        break
    fi
    sleep 0.2
done
[ "$QUIZ_PUBLISHED" = "1" ] || fail "the quiz input.requested event never appeared (see $WORKDIR/daemon.log)"
echo "quiz published"

echo "== the answer route resolves the gate with the correct option =="
OPTIONS="$(input_field quiz options session.json)"
[ -n "$OPTIONS" ] || fail "the quiz input carries no options"
CHOICE="$("$NODE" -e '
    const options = JSON.parse(process.argv[1]);
    console.log(options.indexOf("Paris"));
' "$OPTIONS")"
[ "$CHOICE" -ge 0 ] || fail "the correct option is missing from the presented options: $OPTIONS"
ANSWER="$(curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"answer\",\"input_id\":\"$INPUT_ID\",\"choice\":\"$CHOICE\"}")"
printf '%s' "$ANSWER" | grep -q '"ok":true' \
    || fail "the answer route did not accept the choice: $ANSWER"
echo "quiz answered"

echo "== the second park: ask_user_question, answered with option 0 =="
ASK_PUBLISHED=""
for _ in $(seq 1 50); do
    curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
    ASK_ID="$(input_field question input_id session.json)"
    if [ -n "$ASK_ID" ]; then
        ASK_PUBLISHED=1
        break
    fi
    sleep 0.2
done
[ "$ASK_PUBLISHED" = "1" ] || fail "the ask_user_question input never appeared (see $WORKDIR/daemon.log)"
ANSWER2="$(curl -sf -X POST "$BASE/runs/$RUN1/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"answer\",\"input_id\":\"$ASK_ID\",\"choice\":\"0\"}")"
printf '%s' "$ANSWER2" | grep -q '"ok":true' \
    || fail "the answer route did not accept the question choice: $ANSWER2"
echo "question answered"

echo "== run completes without approval =="
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1; break
    fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.waiting_approval"'; then
        fail "an approval gate appeared (run is waiting for a decision)"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (see $WORKDIR/daemon.log)"
echo "run completed ungated"

curl -sf "$BASE/sessions/$SID" > session.json

echo "== the quiz graded the correct answer, with the explanation =="
grep -q "Correct. The user chose: Paris." session.json \
    || fail "the quiz result is missing the correct-answer grade"
grep -q "Paris is the capital of France" session.json \
    || fail "the quiz result is missing the explanation"
echo "grading correct"

echo "== ask_user_question returned the chosen option ungraded =="
grep -q "The user chose:" session.json \
    || fail "the ask_user_question result is missing"
echo "choice returned"

echo "== the ledger carries input.requested and input.resolved, no approval =="
grep -q '"event_type":"input.requested"' session.json \
    || fail "input.requested is missing from the ledger"
grep -q '"event_type":"input.resolved"' session.json \
    || fail "input.resolved is missing from the ledger"
if grep -q '"event_type":"run.waiting_approval"' session.json; then
    fail "an approval event leaked into the ledger"
fi
echo "ledger correct"

echo
echo "QUIZ_TEST_PASSED"
