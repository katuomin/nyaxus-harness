#!/usr/bin/env bash
# prompt_edit_test.sh - prompt.edit: gated self-edit + drift + frozen guard.
#
# Covers:
#   - a new session pins the resolved pack snapshot (pack_snapshot_id +
#     pack_revision on the session; one shared snapshot row per revision)
#   - a prompt.edit call stops at the approval gate, and the approval event
#     carries the module's current text (old_content) for the UI diff
#   - denial leaves the pack unchanged (content and revision)
#   - approval rewrites the module, bumps the pack revision, and records a
#     prompt.pack_edited audit event with the model's reason
#   - drift guard: the original session keeps its pinned snapshot, while a
#     new session adopts the new revision
#   - frozen-section guard: an approved edit that drops a `<!-- frozen -->`
#     span is rejected with a human-readable reason; the module and the
#     pack revision stay unchanged
#
# Usage: harness/tests/prompt_edit_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/build-mingw/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
PROVIDER_PORT=9021
DAEMON_PORT=9022
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
MOCK="http://127.0.0.1:$PROVIDER_PORT"

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

# JSON field extraction via node (the daemon's objects are nested).
# $2 is a JS function expression over the parsed body, passed through the
# environment so no shell/JS quoting gets in the way.
json_field() {
    curl -sf "$1" | PE_EXPR="$2" "$NODE" -e '
        let d = "";
        const expr = new Function("body", "return (" + process.env.PE_EXPR + ")(body);");
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const v = expr(JSON.parse(d));
            process.stdout.write(v === undefined || v === null ? "" : String(v));
        });
    '
}

MOCK_PORT="$PROVIDER_PORT" "$NODE" "$ROOT/harness/tests/mock_prompt_edit_provider.mjs" > provider.log 2>&1 &
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

echo "== pack + profile =="
# The core module carries an author-declared frozen span (a pinned voice
# line between `<!-- frozen -->` markers): the v2 edit must keep it
# verbatim, and the v3 edit (which drops it) must be rejected.
V1_CORE=$'v1 core text\n<!-- frozen -->\nPinned: I speak in short, dry sentences.\n<!-- /frozen -->'
V2_CORE=$'v2 core text\n<!-- frozen -->\nPinned: I speak in short, dry sentences.\n<!-- /frozen -->'
PACK='{"id":"petest","name":"pe test","description":"prompt edit test","files":[
  {"path":"manifest.json","content":"{\"pack\":\"petest\",\"manifest_version\":1,\"modules\":[{\"id\":\"core\",\"path\":\"core.md\",\"category\":\"identity\"},{\"id\":\"voice\",\"path\":\"voice.md\",\"category\":\"policy\"}]}"},
  {"path":"core.md","content":"v1 core text\n<!-- frozen -->\nPinned: I speak in short, dry sentences.\n<!-- /frozen -->"},
  {"path":"voice.md","content":"v1 voice text"}]}'
curl -sf -X POST "$BASE/prompt-packs" -H 'Content-Type: application/json' -d "$PACK" > /dev/null \
    || fail "pack creation failed"
curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"pe-test","name":"pe test","prompt_pack_id":"petest","prompt_plan":["core","voice"]}' > /dev/null \
    || fail "profile creation failed"
PACK_REV0="$(json_field "$BASE/prompt-packs/petest" 'd => d.revision')"
[ -n "$PACK_REV0" ] || fail "could not read the initial pack revision"
PACK_REV1=$((PACK_REV0 + 1))
echo "pack + profile created (initial revision $PACK_REV0)"

SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"pe-test"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

start_run() {  # $1 = session id, $2 = message
    curl -sf -X POST "$BASE/sessions/$1/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$1\",\"message\":\"$2\"}" \
        | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p'
}

wait_for() {  # $1 = session id, $2 = event type
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$1" 2>/dev/null | grep -q "\"event_type\":\"$2\""; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

last_approval_id() {  # $1 = session id
    curl -sf "$BASE/sessions/$1" \
        | grep -o '"approval_id":"[^"]*"' | tail -1 \
        | sed 's/"approval_id":"//;s/"$//'
}

echo "== run 1: the session pins the pack snapshot =="
RUN1="$(start_run "$SID" "hello")"
[ -n "$RUN1" ] || fail "run 1 did not start"
wait_for "$SID" "run.completed" || fail "run 1 did not complete (see $WORKDIR/daemon.log)"
SNAP_ID="$(json_field "$BASE/sessions/$SID" 'd => d.pack_snapshot_id')"
SNAP_REV="$(json_field "$BASE/sessions/$SID" 'd => d.pack_revision')"
[ -n "$SNAP_ID" ] && [ "$SNAP_ID" != "0" ] || fail "session has no pack snapshot pin"
[ "$SNAP_REV" = "$PACK_REV0" ] || fail "session pinned to wrong revision: $SNAP_REV (want $PACK_REV0)"
SNAP_COUNT="$(json_field "$BASE/prompt-packs/petest/snapshots" 'd => d.snapshots.length')"
[ "$SNAP_COUNT" = "1" ] || fail "expected exactly one snapshot, got $SNAP_COUNT"
echo "pinned to snapshot v1 (shared row, not a per-session copy)"

echo "== run 2: prompt.edit is denied; the pack is unchanged =="
RUN2="$(start_run "$SID" "edit please")"
[ -n "$RUN2" ] || fail "run 2 did not start"
wait_for "$SID" "run.waiting_approval" || fail "run 2 did not reach the approval gate"
SESS_JSON="$(curl -sf "$BASE/sessions/$SID")"
printf '%s' "$SESS_JSON" | grep -q '"old_content":"v1 core text' \
    || fail "approval event lacks old_content (the UI diff source)"
APPROVAL_ID="$(last_approval_id "$SID")"
[ -n "$APPROVAL_ID" ] || fail "no approval id"
curl -sf -X POST "$BASE/runs/$RUN2/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"deny\",\"approval_id\":\"$APPROVAL_ID\",\"scope\":\"once\"}" > /dev/null \
    || fail "deny command failed"
wait_for "$SID" "run.completed" || fail "denied run did not complete"
CORE="$(json_field "$BASE/prompt-packs/petest" 'd => (d.files.find(f => f.path === "core.md") || {}).content')"
[ "$CORE" = "$V1_CORE" ] || fail "denied edit changed the module: $CORE"
PACK_REV="$(json_field "$BASE/prompt-packs/petest" 'd => d.revision')"
[ "$PACK_REV" = "$PACK_REV0" ] || fail "denied edit bumped the pack revision: $PACK_REV"
echo "denied: module and revision unchanged"

echo "== run 3: prompt.edit is approved; the pack updates =="
RUN3="$(start_run "$SID" "edit please again")"
[ -n "$RUN3" ] || fail "run 3 did not start"
wait_for "$SID" "run.waiting_approval" || fail "run 3 did not reach the approval gate"
APPROVAL_ID="$(last_approval_id "$SID")"
[ -n "$APPROVAL_ID" ] || fail "no approval id"
curl -sf -X POST "$BASE/runs/$RUN3/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APPROVAL_ID\",\"scope\":\"once\"}" > /dev/null \
    || fail "approve command failed"
wait_for "$SID" "run.completed" || fail "approved run did not complete"
CORE="$(json_field "$BASE/prompt-packs/petest" 'd => (d.files.find(f => f.path === "core.md") || {}).content')"
[ "$CORE" = "$V2_CORE" ] || fail "approved edit did not land: $CORE"
PACK_REV="$(json_field "$BASE/prompt-packs/petest" 'd => d.revision')"
[ "$PACK_REV" = "$PACK_REV1" ] || fail "approval did not bump the pack revision: $PACK_REV (want $PACK_REV1)"
SESS_JSON="$(curl -sf "$BASE/sessions/$SID")"
printf '%s' "$SESS_JSON" | grep -q '"event_type":"prompt.pack_edited"' \
    || fail "no prompt.pack_edited audit event"
printf '%s' "$SESS_JSON" | grep -q '"reason":"test edit"' \
    || fail "audit event lacks the model's reason"
echo "approved: module v2, revision $PACK_REV1, audited with reason"

echo "== run 4: the original session keeps its pinned snapshot =="
RUN4="$(start_run "$SID" "still here")"
[ -n "$RUN4" ] || fail "run 4 did not start"
wait_for "$SID" "run.completed" || fail "run 4 did not complete"
STATS="$(curl -sf "$MOCK/stats")"
printf '%s' "$STATS" | grep -q "v1 core text" \
    || fail "pinned session no longer sees its v1 module"
if printf '%s' "$STATS" | grep -q "v2 core text"; then
    fail "drift guard broken: the pinned session saw the v2 module"
fi
echo "drift guard held: the session is still on v1"

echo "== a new session adopts the new revision =="
SID2="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"pe-test"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no second session id"
RUN5="$(start_run "$SID2" "fresh session")"
[ -n "$RUN5" ] || fail "run 5 did not start"
wait_for "$SID2" "run.completed" || fail "run 5 did not complete"
SNAP_REV2="$(json_field "$BASE/sessions/$SID2" 'd => d.pack_revision')"
[ "$SNAP_REV2" = "$PACK_REV1" ] || fail "new session pinned to wrong revision: $SNAP_REV2 (want $PACK_REV1)"
STATS="$(curl -sf "$MOCK/stats")"
printf '%s' "$STATS" | grep -q "v2 core text" \
    || fail "new session did not adopt the v2 module"
SNAP_COUNT="$(json_field "$BASE/prompt-packs/petest/snapshots" 'd => d.snapshots.length')"
[ "$SNAP_COUNT" = "2" ] || fail "expected one shared snapshot per revision, got $SNAP_COUNT"
echo "new session adopted v2 (two shared snapshots, one per revision)"

echo "== run 6: an approved edit that drops the frozen section is rejected =="
RUN6="$(start_run "$SID" "drop the pinned line")"
[ -n "$RUN6" ] || fail "run 6 did not start"
wait_for "$SID" "run.waiting_approval" || fail "run 6 did not reach the approval gate"
APPROVAL_ID="$(last_approval_id "$SID")"
[ -n "$APPROVAL_ID" ] || fail "no approval id"
curl -sf -X POST "$BASE/runs/$RUN6/commands" -H 'Content-Type: application/json' \
    -d "{\"command\":\"approve\",\"approval_id\":\"$APPROVAL_ID\",\"scope\":\"once\"}" > /dev/null \
    || fail "approve command failed"
wait_for "$SID" "run.completed" || fail "frozen-violating run did not complete"
SESS_JSON="$(curl -sf "$BASE/sessions/$SID")"
printf '%s' "$SESS_JSON" | grep -q '"result":"Error: frozen section(s) missing from the new content' \
    || fail "tool result lacks the frozen-section rejection"
CORE="$(json_field "$BASE/prompt-packs/petest" 'd => (d.files.find(f => f.path === "core.md") || {}).content')"
[ "$CORE" = "$V2_CORE" ] || fail "frozen-violating edit changed the module: $CORE"
PACK_REV="$(json_field "$BASE/prompt-packs/petest" 'd => d.revision')"
[ "$PACK_REV" = "$PACK_REV1" ] || fail "frozen-violating edit bumped the pack revision: $PACK_REV (want $PACK_REV1)"
echo "rejected: frozen span missing -> human-readable error, module and revision unchanged"

echo
echo "PROMPT_EDIT_TEST_PASSED"
