#!/usr/bin/env bash
# group_pack_pin_test.sh - pack-pin semantics for group sessions
# (docs/group-chat.md). Speakers share ONE session, so a group turn must
# resolve the prompt pack per SPEAKER, never through the session pin: a
# pinned snapshot document was resolved for another speaker's plan and
# name (the header line is "profile: <label>").
#
# Asserts, for a two-speaker group whose profiles share one pack:
#   - each turn's recorded provider request carries the pack document
#     resolved for ITS speaker ("profile: gpp-a" on alice's turn,
#     "profile: gpp-b" on bob's) and never the other speaker's
#   - both resolved documents are frozen as shared snapshot rows (one per
#     speaker, dedupe by content)
#   - the group session carries NO pack pin (pack_snapshot_id stays 0 -
#     pinning per turn would just flip between speakers as they alternate)
#
# Usage: harness/tests/group_pack_pin_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9067
DAEMON_PORT=9068
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
MOCK="http://127.0.0.1:$MOCK_PORT"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# run_json <script> <arg> <url> <fail-message>
run_json() { "$NODE" -e "$1" "$2" < <(curl -sf "$3") || fail "$4"; }

e2e_port_free "$DAEMON_PORT"
e2e_port_free "$MOCK_PORT"

echo "== launch mock provider + daemon =="
MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_group_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd "$WORKDIR" && exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 60); do
    if curl -sf "$BASE/health" > /dev/null 2>&1; then break; fi
    sleep 0.25
done
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up"
echo "healthy"

echo "== pack + two pack-bound speaker profiles =="
PACK='{"id":"gppin","name":"gpp in","description":"group pack pin probe","files":[
  {"path":"manifest.json","content":"{\"pack\":\"gppin\",\"manifest_version\":1,\"modules\":[{\"id\":\"core\",\"path\":\"core.md\",\"category\":\"identity\",\"required\":true}]}"},
  {"path":"core.md","content":"shared core text for the pack-pin probe"}]}'
curl -sf -X POST "$BASE/prompt-packs" -H 'Content-Type: application/json' -d "$PACK" > /dev/null \
    || fail "pack creation failed"
curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"gpp-a","name":"gpp-a","prompt_pack_id":"gppin","prompt_plan":["core"]}' > /dev/null \
    || fail "profile gpp-a creation failed"
curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"gpp-b","name":"gpp-b","prompt_pack_id":"gppin","prompt_plan":["core"]}' > /dev/null \
    || fail "profile gpp-b creation failed"
echo "pack + profiles created"

echo "== group session: budget 1 via the per-session override =="
SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"gpp-a","speakers":["gpp-a","gpp-b"]}' \
    | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no group session id"
curl -sf -X POST "$BASE/sessions/$SID/group-rounds" -H 'Content-Type: application/json' \
    -d '{"rounds":1}' > /dev/null || fail "rounds override failed"

RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"pick a launch date\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN" ] || fail "group run did not start"

for _ in $(seq 1 100); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"group.completed"'; then
        break
    fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"group.completed"' \
    || fail "group run did not complete (see $WORKDIR/daemon.log)"
echo "group completed (alice speaks, bob abstains, budget closes)"

echo "== per-speaker pack resolution in the recorded requests =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const reqs = JSON.parse(d);
        const groupReqs = reqs.filter((req) => ((req.messages) || []).some(
            (m) => String(m.content || "").includes("[GROUP]")));
        if (groupReqs.length !== 2) {
            console.error("expected 2 group turns, got " + groupReqs.length);
            process.exit(1);
        }
        for (const req of groupReqs) {
            const sys = (req.messages || []).map((m) => String(m.content || "")).join("\n");
            const self = (sys.match(/You are (\S+),/) || [])[1];
            if (!self) {
                console.error("could not read the speaker from the [GROUP] framing:\n" + sys.slice(0, 400));
                process.exit(1);
            }
            if (!sys.includes("profile: " + self + "\n")) {
                console.error(self + "s request must carry the pack document resolved for " + self);
                process.exit(1);
            }
            const other = self === "gpp-a" ? "gpp-b" : "gpp-a";
            if (sys.includes("profile: " + other + "\n")) {
                console.error(self + "s request carried the other speakers pack document (session-pin reuse)");
                process.exit(1);
            }
            if (!sys.includes("[PROMPT PACK]\npack: gppin\nprofile: " + self + "\n")) {
                console.error("the pack header must resolve per speaker for " + self);
                process.exit(1);
            }
        }
        console.log("per-speaker pack resolution verified (each turn reads its own document)");
    });' "" "$MOCK/requests" "per-speaker pack resolution"

echo "== snapshots frozen per distinct document; session pin stays empty =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const body = JSON.parse(d);
        const snaps = body.snapshots || [];
        if (snaps.length !== 2) {
            console.error("expected one snapshot per distinct resolved document (2), got " + snaps.length);
            console.error("body: " + d.slice(0, 600));
            process.exit(1);
        }
        // The list route omits the document; the per-speaker fingerprints
        // (prompts differ by the header "profile: <label>" line) prove that
        // each speaker resolved document was frozen separately. The
        // per-snapshot route below deep-checks both documents.
        const fps = snaps.map((s) => (s.fingerprint || "").toLowerCase());
        const distinct = new Set(fps);
        if (distinct.size !== 2) {
            console.error("expected two distinct fingerprints, got " + JSON.stringify(fps));
            console.error("body: " + d.slice(0, 600));
            process.exit(1);
        }
        console.log("both documents frozen as shared snapshot rows (two distinct fingerprints):");
        console.log(snaps.map((s) => s.fingerprint).join("\n"));
    });' "" "$BASE/prompt-packs/gppin/snapshots" "snapshot rows"

SNAP_ID="$(curl -sf "$BASE/sessions/$SID" | sed -n 's/.*"pack_snapshot_id":\([0-9]*\).*/\1/p')"
[ "$SNAP_ID" = "0" ] || fail "group session must not pin a pack snapshot (got $SNAP_ID)"
echo "group session carries no pack pin (pin would flip between speakers)"

echo "GROUP_PACK_PIN_TEST_PASSED"
