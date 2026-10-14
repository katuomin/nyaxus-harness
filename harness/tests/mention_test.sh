#!/usr/bin/env bash
# mention_test.sh - @-mention path references and terminal output
# truncation: @-paths in the user's message (explicit composer spans plus
# hand-typed tokens) are verified on the daemon side and injected as a
# per-turn note; oversized terminal output is head+tail truncated with the
# exact dropped byte count.
#
# Covers:
#   - the referenced-paths note lists existing files (with size),
#     directories, and missing paths, including a spaced path that only the
#     explicit mention carries
#   - a >256 KiB terminal output comes back head+tail with the truncation
#     marker (the result stays inside the context window)
#
# Usage: harness/tests/mention_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9037
DAEMON_PORT=9038
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
# MSYS paths, so the workspace and fixtures live at the Windows form of the
# temp dir.
WORKDIR_WIN="$(cygpath -m "$WORKDIR")"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# Fixtures for the mention note: an existing file, an existing directory,
# a missing file (referenced only), and a file whose name has a space.
mkdir -p "$WORKDIR/subdir"
printf 'hello target\n' > "$WORKDIR/target.txt"
printf 'inner\n' > "$WORKDIR/subdir/inner.txt"
printf 'spaced\n' > "$WORKDIR/no space file.txt"
# >256 KiB so `awk '{print}' big.txt` crosses the terminal output cap.
"$NODE" -e 'require("fs").writeFileSync("big.txt", "x".repeat(300 * 1024) + "\n")'

MENTION_REQUESTS_FILE="$WORKDIR_WIN/requests.log" \
    MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_mention_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model \
    --workspace "$WORKDIR_WIN" > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

# 1 when the given run has a terminal event in the ledger.
run_finished() {
    "$NODE" -e '
        const j = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
        const done = (j.events || []).some(
            (e) => e.run_id === process.argv[2] &&
                (e.event_type === "run.completed" || e.event_type === "run.failed" ||
                 e.event_type === "run.cancelled"));
        console.log(done ? "1" : "");
    ' "$1" "$2"
}

# The approval_id of the first approval.requested event of a given run.
approval_field() {
    "$NODE" -e '
        const j = JSON.parse(require("fs").readFileSync(process.argv[1], "utf8"));
        const ev = (j.events || []).find(
            (e) => e.event_type === "approval.requested" && e.run_id === process.argv[2]);
        console.log(ev && ev.payload ? String(ev.payload[process.argv[3]] || "") : "");
    ' "$1" "$2" "$3"
}

echo "== turn one: @-mentions are verified and injected =="
MESSAGE="check out @$WORKDIR_WIN/target.txt and @$WORKDIR_WIN/subdir, plus @$WORKDIR_WIN/missing.txt and @$WORKDIR_WIN/no space file.txt"
BODY="$("$NODE" -e '
    console.log(JSON.stringify({
        session_id: process.argv[1],
        message: process.argv[2],
        mentions: [process.argv[3], process.argv[4]],
    }));
' "$SID" "$MESSAGE" "$WORKDIR_WIN/no space file.txt" "$WORKDIR_WIN/target.txt")"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "$BODY" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "the mention turn did not start"
for _ in $(seq 1 250); do
    curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
    [ "$(run_finished session.json "$RUN1")" = "1" ] && break
    sleep 0.2
done
[ "$(run_finished session.json "$RUN1")" = "1" ] || fail "the mention turn did not finish (see $WORKDIR/daemon.log)"
grep -q "run.completed" session.json || fail "the mention turn did not complete (see $WORKDIR/daemon.log)"
echo "turn completed"

NOTE="$( "$NODE" -e '
    const lines = require("fs").readFileSync(process.argv[1], "utf8").split("\n").filter(Boolean);
    for (let i = lines.length - 1; i >= 0; i--) {
        const body = JSON.parse(lines[i]);
        const msg = (body.messages || []).find(
            (m) => m.role === "system" && String(m.content).startsWith("Paths referenced with @"));
        if (msg) { console.log(String(msg.content)); process.exit(0); }
    }
    console.log("");
' "$WORKDIR_WIN/requests.log" )"
[ -n "$NOTE" ] || fail "the referenced-paths note was not injected into the prompt (see $WORKDIR/requests.log)"
printf '%s' "$NOTE" | grep -q "target.txt - file," || fail "the existing file is not marked as a file: $NOTE"
printf '%s' "$NOTE" | grep -q "subdir - directory" || fail "the existing directory is not marked: $NOTE"
printf '%s' "$NOTE" | grep -q "missing.txt - not found" || fail "the missing file is not marked not found: $NOTE"
printf '%s' "$NOTE" | grep -q "no space file.txt - file," \
    || fail "the spaced path (explicit mention) is not marked as a file: $NOTE"
echo "note correct"

echo "== turn two: oversized terminal output is head+tail truncated =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "$("$NODE" -e '
        console.log(JSON.stringify({ session_id: process.argv[1], message: "dump big" }));
    ' "$SID")" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "the truncation turn did not start"
# terminal is approval-gated: approve it once the gate publishes.
APPROVAL_ID=""
for _ in $(seq 1 200); do
    curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
    APPROVAL_ID="$(approval_field session.json "$RUN2" approval_id)"
    if [ -n "$APPROVAL_ID" ]; then break; fi
    sleep 0.2
done
[ -n "$APPROVAL_ID" ] || fail "the terminal approval never appeared (see $WORKDIR/daemon.log)"
curl -sf -X POST "$BASE/runs/$RUN2/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APPROVAL_ID\",\"scope\":\"once\"}" > /dev/null
for _ in $(seq 1 250); do
    curl -sf "$BASE/sessions/$SID" > session.json 2>/dev/null || true
    [ "$(run_finished session.json "$RUN2")" = "1" ] && break
    sleep 0.2
done
[ "$(run_finished session.json "$RUN2")" = "1" ] || fail "the truncation turn did not finish (see $WORKDIR/daemon.log)"
grep -q "run.completed" session.json || fail "the truncation turn did not complete (see $WORKDIR/daemon.log)"
grep -q "bytes of output omitted" session.json \
    || fail "the reduced terminal output carries no reduction marker"
# The marker reports a positive dropped byte count (300 KiB in, reduced
# to head+tail at the 32 KiB target).
DROPPED="$(grep -o '[0-9]* bytes of output omitted' session.json | head -1 | grep -o '^[0-9]*')"
[ -n "$DROPPED" ] && [ "$DROPPED" -gt 0 ] || fail "the truncation marker carries no dropped byte count"
echo "truncation marker correct ($DROPPED bytes dropped)"

echo
echo "MENTION_TEST_PASSED"
