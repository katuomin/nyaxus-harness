#!/usr/bin/env bash
# title_digit_mask_test.sh - digit-free openers get no digit-leading title.
#
# Covers:
#   - the title model's "lead with a number" prior is blocked at the source:
#     a message with no ASCII digits must not produce a title whose first
#     character is a digit (the pre-fix model led with a digit on ~70% of
#     digit-free openers, measured; the training data does it in <1% of rows)
#   - the mask does not over-block: an opener that DOES carry digits still
#     gets a title (a grounded digit lead like "10GBe ..." stays legal)
#   - script-leak guard: a pure-ASCII opener never gets a non-ASCII title
#     (Chinese leaks on short/ambiguous input, other scripts in degenerate
#     outputs; the daemon falls back to the faithful truncation)
#
# Gated on the real int8 title model (NYAXUS_TITLE_MODEL), like
# title_retitle_test.sh. Fails on a pre-fix binary: at least one of the
# digit-free openers below produced a digit-leading title (the model is
# deterministic, greedy).
#
# Usage: harness/tests/title_digit_mask_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8995
DAEMON_PORT=8996
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

# Script-leak guard: a pure-ASCII message must not get a non-ASCII title
# (the base model leaks Chinese on short/ambiguous input and occasionally
# other scripts in degenerate outputs; the daemon falls back to the faithful
# truncation instead).
has_non_ascii() {
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            process.exit(/[^\x00-\x7f]/.test(d) ? 1 : 0);
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

# All 100+ chars (so the title model is invoked) and free of ASCII digits
# (so no leading digit can be grounded). Pre-fix, the model led with a
# digit on 13 of these 14 (measured).
MSG_NO_DIGIT=(
    "the doctor said the biopsy results were not good and i have to wait two weeks for the full report, how do i tell my family without giving them false hope"
    "i had a dream my dead grandmother was angry at me and i woke up crying, what does that even mean"
    "my dad hit my mom for twenty years and last night i caught myself wanting to hit him back when he raised his voice, i am scared of what that means about me"
    "i keep thinking about jumping off the bridge over the river, i have driven past it a hundred times, and i do not know why it keeps coming back"
    # Pre-fix: degenerate all-digit title; with the digit mask blocked the
    # model's second-best is a non-ASCII script leak - the guard must catch it.
    "my sister moved back home into the room next to mine and i keep thinking about her more than i should, i do not know what to do about it"
)

echo "== digit-free openers: no digit-leading title =="
for MSG in "${MSG_NO_DIGIT[@]}"; do
    case "$MSG" in
        *[0-9]*) fail "test bug: message contains a digit: $MSG" ;;
    esac
    SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$SID" ] || fail "no session id"
    RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$MSG\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$RUN" ] || fail "run did not start"
    TITLE="$(title_of "$SID")"
    [ -n "$TITLE" ] || fail "no title for digit-free opener (model failed?)"
    case "$TITLE" in
        [0-9]*) fail "digit-leading title from a digit-free opener: $TITLE" ;;
    esac
    has_non_ascii "$TITLE" || fail "non-ASCII title from a pure-ASCII opener: $TITLE"
    echo "ok: $TITLE"
done

echo "== digit-carrying opener: mask does not over-block =="
MSG_DIGIT="i am trying to set up a home lab with two esxi hosts and a 10gbe switch, but the vmotion migration keeps failing with a checksum mismatch, any ideas"
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"$MSG_DIGIT\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "run did not start"
TITLE="$(title_of "$SID")"
[ -n "$TITLE" ] || fail "no title for digit-carrying opener (mask over-blocked?)"
has_non_ascii "$TITLE" || fail "non-ASCII title from a pure-ASCII opener: $TITLE"
echo "ok: $TITLE"

echo
echo "TITLE_DIGIT_MASK_TEST_PASSED"
