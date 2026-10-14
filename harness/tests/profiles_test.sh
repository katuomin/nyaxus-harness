#!/usr/bin/env bash
# End-to-end check for the profiles API against the daemon.
#
# A mock provider answers with a single text turn that echoes the first
# system message it received, or reports that none was sent. The test:
#   - lists profiles and expects the seeded "orchestrator"
#   - creates a custom profile with an identity prompt (409 on duplicate)
#   - runs a default-profile session and asserts only the [CLOCK] block
#     reached the provider
#   - selects the custom profile and asserts the identity prompt reached
#     the provider, with the profile id in run.started
#   - creates a session directly on the custom profile (POST /sessions)
#   - rejects an unknown profile selection with 404
#   - updates the profile and asserts the new prompt reaches the provider
#   - deletes the profile; protects the seeded default from deletion
#
# Usage: harness/tests/profiles_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8907
DAEMON_PORT=8908
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

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== models endpoint lists the configured model =="
MODELS="$(curl -sf "$BASE/models")"
printf '%s' "$MODELS" | grep -q '"mock-model"' || fail "models endpoint missing the configured model: $MODELS"
printf '%s' "$MODELS" | grep -q '"default":"mock-model"' || fail "models endpoint missing the default marker"
echo "mock-model listed"

echo "== list profiles (expect the seeded orchestrator) =="
PROFILES="$(curl -sf "$BASE/profiles")"
printf '%s' "$PROFILES" | grep -q '"id":"orchestrator"' || fail "seeded orchestrator profile missing"
echo "orchestrator present"

echo "== create a custom profile with an identity prompt =="
CREATED="$(curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"poet","name":"poet","description":"terse poet","identity_prompt":"You are a terse poet.","base_model":"mock-model"}')"
printf '%s' "$CREATED" | grep -q '"id":"poet"' || fail "profile create did not echo the profile"
echo "poet created"

echo "== duplicate create must 409 =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"poet","name":"poet"}')"
[ "$CODE" = "409" ] || fail "duplicate profile create returned $CODE, want 409"
echo "409 on duplicate"

echo "== default session: policy tier first, [CLOCK] at the volatile tail =="
SID_A="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_A" ] || fail "no session id"
RUN_A="$(curl -sf -X POST "$BASE/sessions/$SID_A/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_A\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_A" ] || fail "run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_A" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "default-profile run did not complete"
ANSWER_A="$(curl -sf "$BASE/sessions/$SID_A" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const asst = (s.messages || []).filter((m) => m.role === "assistant");
        process.stdout.write(asst.length ? asst[asst.length - 1].content : "");
    });
')"
case "$ANSWER_A" in
    "system: You can read past conversations"*) echo "clean: stable tier first, clock at the tail" ;;
    *) fail "default profile sent an unexpected first system message: $ANSWER_A" ;;
esac
printf '%s' "$ANSWER_A" | grep -q "\[CLOCK\]" || fail "clock block missing (it belongs at the tail now): $ANSWER_A"
printf '%s' "$ANSWER_A" | grep -q "terse poet" && fail "identity leaked into the default session: $ANSWER_A"

echo "== select the custom profile on a second session =="
SID_B="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_B" ] || fail "no session id"
SEL="$(curl -sf -X POST "$BASE/sessions/$SID_B/profile" -H 'Content-Type: application/json' \
    -d '{"profile":"poet"}')"
printf '%s' "$SEL" | grep -q '"ok":true' || fail "profile select failed: $SEL"
echo "poet selected"

echo "== custom session: the identity prompt must reach the provider =="
RUN_B="$(curl -sf -X POST "$BASE/sessions/$SID_B/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_B\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_B" ] || fail "run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_B" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "custom-profile run did not complete"
ANSWER_B="$(curl -sf "$BASE/sessions/$SID_B" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const asst = (s.messages || []).filter((m) => m.role === "assistant");
        process.stdout.write(asst.length ? asst[asst.length - 1].content : "");
    });
')"
case "$ANSWER_B" in
    "system: You are a terse poet."*) ;;
    *) fail "identity prompt did not reach the provider: $ANSWER_B" ;;
esac
printf '%s' "$ANSWER_B" | grep -q "\[CLOCK\]" || fail "clock block missing from the custom session: $ANSWER_B"
echo "identity prompt delivered"

echo "== run.started must carry the profile id =="
SNAP_B="$(curl -sf "$BASE/sessions/$SID_B")"
printf '%s' "$SNAP_B" | grep -q '"profile":"poet"' || fail "run.started missing the profile id"
echo "profile id in run.started"

echo "== create a session directly on the custom profile =="
SID_C="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"poet"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_C" ] || fail "no session id"
RUN_C="$(curl -sf -X POST "$BASE/sessions/$SID_C/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_C\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_C" ] || fail "run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_C" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "direct-profile run did not complete"
ANSWER_C="$(curl -sf "$BASE/sessions/$SID_C" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const asst = (s.messages || []).filter((m) => m.role === "assistant");
        process.stdout.write(asst.length ? asst[asst.length - 1].content : "");
    });
')"
case "$ANSWER_C" in
    "system: You are a terse poet."*) ;;
    *) fail "direct-profile session lost the identity prompt: $ANSWER_C" ;;
esac
echo "direct-profile session delivered"

echo "== unknown profile selection must 404 =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_B/profile" -H 'Content-Type: application/json' \
    -d '{"profile":"ghost"}')"
[ "$CODE" = "404" ] || fail "unknown profile select returned $CODE, want 404"
echo "404 on unknown profile"

echo "== update the custom profile (name, prompt, temperature) =="
UPDATED="$(curl -sf -X POST "$BASE/profiles/poet" -H 'Content-Type: application/json' \
    -d '{"name":"bard","description":"updated bard","identity_prompt":"You are a laconic bard.","base_model":"mock-model","temperature":0.2}')"
printf '%s' "$UPDATED" | grep -q '"name":"bard"' || fail "profile update did not echo the new name"
printf '%s' "$UPDATED" | grep -q '"identity_prompt":"You are a laconic bard."' || fail "profile update did not echo the new prompt"
echo "poet updated to bard"

echo "== the updated identity prompt must reach the provider =="
SID_D="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"poet"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_D" ] || fail "no session id"
RUN_D="$(curl -sf -X POST "$BASE/sessions/$SID_D/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_D\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_D" ] || fail "run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_D" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "updated-profile run did not complete"
ANSWER_D="$(curl -sf "$BASE/sessions/$SID_D" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const asst = (s.messages || []).filter((m) => m.role === "assistant");
        process.stdout.write(asst.length ? asst[asst.length - 1].content : "");
    });
')"
case "$ANSWER_D" in
    "system: You are a laconic bard."*) ;;
    *) fail "updated identity prompt did not reach the provider: $ANSWER_D" ;;
esac
echo "updated prompt delivered"

echo "== update must 404 on an unknown profile =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/ghost" -H 'Content-Type: application/json' \
    -d '{"name":"ghost"}')"
[ "$CODE" = "404" ] || fail "unknown profile update returned $CODE, want 404"
echo "404 on unknown update"

echo "== delete the custom profile =="
DEL="$(curl -sf -X POST "$BASE/profiles/poet/delete" -d '{}')"
printf '%s' "$DEL" | grep -q '"ok":true' || fail "profile delete failed: $DEL"
AFTER="$(curl -sf "$BASE/profiles")"
printf '%s' "$AFTER" | grep -q '"id":"poet"' && fail "deleted profile still listed"
echo "poet deleted"

echo "== delete must 409 on the default profile =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/orchestrator/delete" -d '{}')"
[ "$CODE" = "409" ] || fail "default profile delete returned $CODE, want 409"
echo "409 on default delete"

echo "== delete must 404 on an unknown profile =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/ghost/delete" -d '{}')"
[ "$CODE" = "404" ] || fail "unknown profile delete returned $CODE, want 404"
echo "404 on unknown delete"

echo "== rename a profile slug =="
curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"old-slug","name":"old slug","identity_prompt":"You are a terse poet.","base_model":"mock-model"}' > /dev/null \
    || fail "rename setup: profile create failed"
SID_E="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"old-slug"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_E" ] || fail "rename setup: no session id"
RUN_E="$(curl -sf -X POST "$BASE/sessions/$SID_E/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_E\",\"message\":\"hello\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_E" ] || fail "rename setup: run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_E" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "rename setup: run did not complete"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/old-slug/rename" \
    -H 'Content-Type: application/json' -d '{"id":"new-slug"}')"
[ "$CODE" = "200" ] || fail "rename returned $CODE, want 200"
PROFILES="$(curl -sf "$BASE/profiles")"
printf '%s' "$PROFILES" | grep -q '"id":"new-slug"' || fail "renamed profile missing from the list"
printf '%s' "$PROFILES" | grep -q '"id":"old-slug"' && fail "old slug still listed"
echo "slug renamed"

echo "== the renamed profile still routes (session reference follows) =="
RUN_F="$(curl -sf -X POST "$BASE/sessions/$SID_E/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_E\",\"message\":\"hello again\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN_F" ] || fail "post-rename run did not start"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_E" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "post-rename run did not complete"
ANSWER_E="$(curl -sf "$BASE/sessions/$SID_E" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const asst = (s.messages || []).filter((m) => m.role === "assistant");
        process.stdout.write(asst.length ? asst[asst.length - 1].content : "");
    });
')"
case "$ANSWER_E" in
    "system: You are a terse poet."*) ;;
    *) fail "post-rename run lost the identity prompt: $ANSWER_E" ;;
esac
echo "renamed profile still routes"

echo "== rename guards =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/new-slug/rename" \
    -H 'Content-Type: application/json' -d '{"id":"orchestrator"}')"
[ "$CODE" = "409" ] || fail "rename onto the default returned $CODE, want 409"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/orchestrator/rename" \
    -H 'Content-Type: application/json' -d '{"id":"other"}')"
[ "$CODE" = "409" ] || fail "renaming the default returned $CODE, want 409"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/ghost/rename" \
    -H 'Content-Type: application/json' -d '{"id":"other"}')"
[ "$CODE" = "404" ] || fail "renaming an unknown profile returned $CODE, want 404"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/new-slug/rename" \
    -H 'Content-Type: application/json' -d '{"id":"bad slug"}')"
[ "$CODE" = "400" ] || fail "invalid slug returned $CODE, want 400"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles/new-slug/rename" \
    -H 'Content-Type: application/json' -d '{"id":"new-slug"}')"
[ "$CODE" = "200" ] || fail "no-op rename returned $CODE, want 200"
echo "rename guards ok"

echo "== vision: unset profile inherits the connection's capability =="
curl -sf -X POST "$BASE/providers" -H 'Content-Type: application/json' \
    -d "{\"name\":\"vision-conn\",\"endpoint\":\"http://127.0.0.1:$MOCK_PORT/v1\",\"model\":\"mock-model\",\"params\":{\"vision\":true}}" > /dev/null \
    || fail "vision setup: provider create failed"
INHERIT="$(curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"inherit-vision","name":"inherit vision","provider":"vision-conn"}')"
printf '%s' "$INHERIT" | grep -q '"vision":null' || fail "unset vision should serialize as null: $INHERIT"
NOVISION="$(curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"no-vision","name":"no vision","provider":"vision-conn","vision":false}')"
printf '%s' "$NOVISION" | grep -q '"vision":false' || fail "explicit false vision missing: $NOVISION"
echo "vision tristate serialized"

IMAGE_DATA_URL="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"

build_image_payload() {
    "$NODE" -e '
        const fs = require("fs");
        fs.writeFileSync(process.argv[1], JSON.stringify({
            session_id: process.argv[2],
            message: "describe this image",
            attachments: [{ name: "pixel.png", kind: "image", mime_type: "image/png", width: 1, height: 1, content: process.argv[3] }],
        }));
    ' "payload_$1.json" "$1" "$IMAGE_DATA_URL" || fail "could not build image payload for $1"
}

echo "== inherited vision accepts an image; explicit false rejects it =="
SID_V="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"inherit-vision"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_V" ] || fail "no vision session id"
build_image_payload "$SID_V"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_V/runs" \
    -H 'Content-Type: application/json' --data-binary @"payload_$SID_V.json")"
[ "$CODE" = "200" ] || fail "inherited-vision image run returned $CODE, want 200"
DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID_V" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "inherited-vision run did not complete"
echo "inherited vision accepted the image"

SID_NV="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"no-vision"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_NV" ] || fail "no no-vision session id"
build_image_payload "$SID_NV"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_NV/runs" \
    -H 'Content-Type: application/json' --data-binary @"payload_$SID_NV.json")"
[ "$CODE" = "400" ] || fail "explicit-false-vision image run returned $CODE, want 400"
echo "explicit false vision rejected the image"

echo
echo "PROFILES_TEST_PASSED"
