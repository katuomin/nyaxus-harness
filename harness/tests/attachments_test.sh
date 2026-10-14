#!/usr/bin/env bash
# End-to-end check for the structured attachments API against the daemon.
#
# A mock provider echoes the last user message it received, so the test can
# verify byte for byte what the daemon sends. The test:
#   - sends a text attachment and asserts it is stored on the user message
#     (clean content, attachment row with name/kind/size/content)
#   - asserts the provider received the formatted block
#     (attached file: NAME + fenced content), not the raw stored text
#   - accepts an attachment-only message (empty text)
#   - rejects an attachment over the 200 kb cap with 413
#   - accepts five attachments and rejects ten with 400
#   - accepts an image for a vision profile and rejects it otherwise
#   - keeps image bytes out of snapshots, exposes a content URL, and returns
#     decoded bytes from that URL
#   - verifies the provider receives an image_url content part
#
# Usage: harness/tests/attachments_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=8911
DAEMON_PORT=8912
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

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_attachment_provider.mjs" > mock.log 2>&1 &
MPID=$!
e2e_port_free "$DAEMON_PORT"
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

wait_done() {
    local sid="$1"
    local done=""
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            done=1
            break
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run in session $sid did not complete"
}

wait_run_done() {
    local sid="$1"
    local rid="$2"
    local done=""
    for _ in $(seq 1 75); do
        if curl -sf "$BASE/sessions/$sid" 2>/dev/null | "$NODE" -e '
            let d = "";
            process.stdin.on("data", (c) => { d += c; });
            process.stdin.on("end", () => {
                const s = JSON.parse(d);
                const rid = process.argv[1];
                const done = (s.events || []).some((event) =>
                    event.run_id === rid && event.event_type === "run.completed");
                process.exit(done ? 0 : 1);
            });
        ' "$rid"; then
            done=1
            break
        fi
        sleep 0.2
    done
    [ "$done" = "1" ] || fail "run $rid in session $sid did not complete"
}

# Prints the last assistant message content of a session.
last_answer() {
    curl -sf "$BASE/sessions/$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const asst = (s.messages || []).filter((m) => m.role === "assistant");
            process.stdout.write(asst.length ? asst[asst.length - 1].content : "");
        });
    '
}

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== text attachment is stored on the user message =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d '{"session_id":"'"$SID"'","message":"summarize this","attachments":[{"name":"notes.txt","kind":"text","content":"hello attachment"}]}')"
printf '%s' "$RUN" | grep -q '"run_id"' || fail "run did not start: $RUN"
wait_done "$SID"
SNAP="$(curl -sf "$BASE/sessions/$SID")"
printf '%s' "$SNAP" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const user = (s.messages || []).find((m) => m.role === "user");
        if (!user) { console.error("no user message"); process.exit(1); }
        const att = (user.attachments || [])[0];
        if (!att) { console.error("user message has no attachment row"); process.exit(1); }
        if (att.name !== "notes.txt") { console.error("bad name: " + att.name); process.exit(1); }
        if (att.kind !== "text") { console.error("bad kind: " + att.kind); process.exit(1); }
        if (Number(att.size) !== "hello attachment".length) { console.error("bad size: " + att.size); process.exit(1); }
        if (att.content !== "hello attachment") { console.error("bad content: " + att.content); process.exit(1); }
        if (user.content.includes("attached file:")) { console.error("stored user content is not clean: " + user.content); process.exit(1); }
    });
' || fail "attachment row missing or wrong"
echo "stored with clean content"

echo "== the formatted block must reach the provider =="
ANSWER="$(last_answer "$SID")"
printf '%s' "$ANSWER" | grep -q 'attached file: notes.txt' || fail "formatted block missing: $ANSWER"
printf '%s' "$ANSWER" | grep -q 'hello attachment' || fail "attachment content missing: $ANSWER"
printf '%s' "$ANSWER" | grep -q 'summarize this' || fail "base message missing: $ANSWER"
echo "block delivered"

echo "== attachment-only message (empty text) is accepted =="
SID_B="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_B" ] || fail "no session id"
RUN_B="$(curl -sf -X POST "$BASE/sessions/$SID_B/runs" -H 'Content-Type: application/json' \
    -d '{"session_id":"'"$SID_B"'","message":"","attachments":[{"name":"solo.txt","kind":"text","content":"standalone"}]}')"
printf '%s' "$RUN_B" | grep -q '"run_id"' || fail "attachment-only run did not start: $RUN_B"
wait_done "$SID_B"
ANSWER_B="$(last_answer "$SID_B")"
printf '%s' "$ANSWER_B" | grep -q 'attached file: solo.txt' || fail "solo attachment not delivered: $ANSWER_B"
echo "attachment-only accepted"

echo "== oversized attachment must 413 =="
"$NODE" -e '
    const fs = require("fs");
    fs.writeFileSync("big.json", JSON.stringify({
        session_id: process.argv[1],
        message: "x",
        attachments: [{ name: "big.txt", kind: "text", content: "a".repeat(201 * 1024) }],
    }));
' "$SID" || fail "could not build oversized payload"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/runs" \
    -H 'Content-Type: application/json' -d @big.json)"
[ "$CODE" = "413" ] || fail "oversized attachment returned $CODE, want 413"
echo "413 on oversized"

echo "== five attachments are accepted, ten must 400 =="
SID_MANY="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_MANY" ] || fail "no session id for attachment count test"
"$NODE" -e '
    const fs = require("fs");
    fs.writeFileSync("five.json", JSON.stringify({
        session_id: process.argv[1],
        message: "five files",
        attachments: [1, 2, 3, 4, 5].map((n) => ({ name: "f" + n + ".txt", kind: "text", content: "c" + n })),
    }));
' "$SID_MANY" || fail "could not build five-attachment payload"
FIVE="$(curl -sf -X POST "$BASE/sessions/$SID_MANY/runs" \
    -H 'Content-Type: application/json' -d @five.json)"
printf '%s' "$FIVE" | grep -q '"run_id"' || fail "five attachments were not accepted: $FIVE"
wait_done "$SID_MANY"
echo "five attachments accepted"
"$NODE" -e '
    const fs = require("fs");
    fs.writeFileSync("many.json", JSON.stringify({
        session_id: process.argv[1],
        message: "ten files",
        attachments: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map((n) => ({ name: "f" + n + ".txt", kind: "text", content: "c" + n })),
    }));
' "$SID_MANY" || fail "could not build ten-attachment payload"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_MANY/runs" \
    -H 'Content-Type: application/json' -d @many.json)"
[ "$CODE" = "400" ] || fail "10 attachments returned $CODE, want 400"
echo "400 on 10 attachments"

echo "== image requires vision and reaches the provider as image_url =="
VISION_PROFILE="$(curl -sf -X POST "$BASE/profiles" -H 'Content-Type: application/json' \
    -d '{"id":"vision-test","name":"vision-test","vision":true,"base_model":"mock-model"}')"
printf '%s' "$VISION_PROFILE" | grep -q '"vision":true' || fail "vision profile was not persisted"
SID_IMAGE="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"vision-test"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_IMAGE" ] || fail "no image session id"
# A valid 1x1 red PNG, so browser image decoding is covered rather than just
# the data-url transport path.
IMAGE_DATA_URL="data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"
"$NODE" -e '
    const fs = require("fs");
    fs.writeFileSync("image.json", JSON.stringify({
        session_id: process.argv[1],
        message: "describe this image",
        attachments: [{
            name: "pixel.png",
            kind: "image",
            mime_type: "image/png",
            width: 1,
            height: 1,
            content: process.argv[2],
        }],
    }));
' "$SID_IMAGE" "$IMAGE_DATA_URL" || fail "could not build image payload"
RUN_IMAGE="$(curl -sf -X POST "$BASE/sessions/$SID_IMAGE/runs" \
    -H 'Content-Type: application/json' -d @image.json)"
printf '%s' "$RUN_IMAGE" | grep -q '"run_id"' || fail "vision image run did not start: $RUN_IMAGE"
wait_done "$SID_IMAGE"
ANSWER_IMAGE="$(last_answer "$SID_IMAGE")"
printf '%s' "$ANSWER_IMAGE" | grep -q 'image_url part received' || \
    fail "provider did not receive an image_url part: $ANSWER_IMAGE"
SNAP_IMAGE="$(curl -sf "$BASE/sessions/$SID_IMAGE")"
printf '%s' "$SNAP_IMAGE" > image-snapshot.json
"$NODE" -e '
    const fs = require("fs");
    const sid = process.argv[1];
    const snapshot = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
    const user = (snapshot.messages || []).find((m) => m.role === "user");
    const att = user && (user.attachments || [])[0];
    if (!att) throw new Error("image attachment missing from snapshot");
    if (!Number.isInteger(Number(att.id)) || Number(att.id) <= 0) throw new Error("bad attachment id");
    if (att.kind !== "image") throw new Error("bad image kind");
    if (att.mime_type !== "image/png") throw new Error("bad image mime type");
    if (Number(att.width) !== 1 || Number(att.height) !== 1) throw new Error("bad image dimensions");
    const encoded = process.argv[3] || "";
    if (!Number(att.size) || Number(att.size) >= encoded.length) throw new Error("image size is not normalized binary size");
    if (Object.prototype.hasOwnProperty.call(att, "content")) throw new Error("image content leaked into snapshot");
    if (att.content_url !== `/api/v1/sessions/${sid}/attachments/${att.id}`) throw new Error("bad image content URL");
    process.stdout.write(String(att.id));
' "$SID_IMAGE" image-snapshot.json "$IMAGE_DATA_URL" > image-attachment-id.txt || fail "image snapshot was not redacted correctly"
ATTACHMENT_ID="$(cat image-attachment-id.txt)"
IMAGE_MESSAGE_ID="$("$NODE" -e '
    const fs = require("fs");
    const snapshot = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
    const user = (snapshot.messages || []).find((m) => m.role === "user");
    if (!user) throw new Error("image user message missing");
    process.stdout.write(String(user.id));
' image-snapshot.json)" || fail "could not find image message id"
CODE="$(curl -s -D image.headers -o image.bin -w '%{http_code}' \
    "$BASE/sessions/$SID_IMAGE/attachments/$ATTACHMENT_ID")"
[ "$CODE" = "200" ] || fail "image content route returned $CODE"
grep -qi '^content-type: image/png' image.headers || fail "image route returned the wrong mime type"
"$NODE" -e '
    const fs = require("fs");
    const bytes = fs.readFileSync(process.argv[1]);
    const expected = Buffer.from("89504e470d0a1a0a", "hex");
    if (!bytes.subarray(0, expected.length).equals(expected)) throw new Error(`wrong image bytes: ${bytes.subarray(0, 8).toString("hex")}`);
' image.bin || fail "image content route returned the wrong bytes"
echo "image snapshot and content route verified"

echo "== identical image re-sent in the same session is stored once =="
command -v python >/dev/null 2>&1 || fail "python not found (needed for the dedup check)"
"$NODE" -e '
    const fs = require("fs");
    fs.writeFileSync("image-again.json", JSON.stringify({
        session_id: process.argv[1],
        message: "same image again",
        attachments: [{
            name: "pixel-copy.png",
            kind: "image",
            mime_type: "image/png",
            width: 1,
            height: 1,
            content: process.argv[2],
        }],
    }));
' "$SID_IMAGE" "$IMAGE_DATA_URL" || fail "could not build duplicate image payload"
RUN_IMAGE2="$(curl -sf -X POST "$BASE/sessions/$SID_IMAGE/runs" \
    -H 'Content-Type: application/json' -d @image-again.json)"
printf '%s' "$RUN_IMAGE2" | grep -q '"run_id"' || fail "duplicate image run did not start: $RUN_IMAGE2"
wait_done "$SID_IMAGE"
SNAP_IMAGE2="$(curl -sf "$BASE/sessions/$SID_IMAGE")"
printf '%s' "$SNAP_IMAGE2" > image-snapshot-2.json
DUPLICATE_ATTACHMENT_ID="$("$NODE" -e '
    const fs = require("fs");
    const s = JSON.parse(fs.readFileSync(process.argv[1], "utf8"));
    const users = (s.messages || []).filter((m) => m.role === "user");
    const last = users[users.length - 1];
    const att = last && (last.attachments || [])[0];
    if (!att) throw new Error("duplicate image attachment missing from snapshot");
    if (Object.prototype.hasOwnProperty.call(att, "content")) throw new Error("image content leaked into snapshot");
    process.stdout.write(String(att.id));
' image-snapshot-2.json)" || fail "could not find duplicate attachment id"
CODE="$(curl -s -D image2.headers -o image2.bin -w '%{http_code}' \
    "$BASE/sessions/$SID_IMAGE/attachments/$DUPLICATE_ATTACHMENT_ID")"
[ "$CODE" = "200" ] || fail "duplicate image content route returned $CODE"
grep -qi '^content-type: image/png' image2.headers || fail "duplicate image route returned the wrong mime type"
cmp -s image.bin image2.bin || fail "duplicate image bytes differ from the original"
DUPLICATE_ATTACHMENT_ID="$DUPLICATE_ATTACHMENT_ID" python - <<'PY' || fail "duplicate image content was not stored once"
import os
import sqlite3

con = sqlite3.connect("file:nyaxus.db?mode=ro", uri=True)
att_id = int(os.environ["DUPLICATE_ATTACHMENT_ID"])
row = con.execute("SELECT content_hash FROM attachments WHERE id = ?", (att_id,)).fetchone()
assert row and row[0], "duplicate attachment has no content hash"
scope = (
    "SELECT session_id FROM messages WHERE id = "
    "(SELECT message_id FROM attachments WHERE id = ?)"
)
session = con.execute(scope, (att_id,)).fetchone()[0]
canonical = con.execute(
    "SELECT COUNT(*) FROM attachments a JOIN messages m ON m.id = a.message_id "
    "WHERE a.content_hash = ? AND a.content != '' AND m.session_id = ?",
    (row[0], session),
).fetchone()[0]
refs = con.execute(
    "SELECT COUNT(*) FROM attachments a JOIN messages m ON m.id = a.message_id "
    "WHERE a.content_hash = ? AND a.content = '' AND m.session_id = ?",
    (row[0], session),
).fetchone()[0]
assert canonical == 1, f"expected exactly one canonical row, got {canonical}"
assert refs >= 1, "expected at least one reference row"
con.close()
PY
echo "identical image stored once, served twice"

CODE="$(curl -s -o /dev/null -w '%{http_code}' \
    "$BASE/sessions/$SID/attachments/$ATTACHMENT_ID")"
[ "$CODE" = "404" ] || fail "cross-session attachment lookup returned $CODE, want 404"
echo "attachment ownership enforced"

"$NODE" -e '
    const fs = require("fs");
    fs.writeFileSync("image-no-vision.json", JSON.stringify({
        session_id: process.argv[1],
        message: "unsupported image",
        attachments: [{ name: "pixel.png", kind: "image", content: process.argv[2] }],
    }));
' "$SID_B" "$IMAGE_DATA_URL" || fail "could not build no-vision image payload"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_B/runs" \
    -H 'Content-Type: application/json' -d @image-no-vision.json)"
[ "$CODE" = "400" ] || fail "image without vision returned $CODE, want 400"
echo "image rejected without vision"

CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_IMAGE/profile" \
    -H 'Content-Type: application/json' -d '{"profile":"orchestrator"}')"
[ "$CODE" = "409" ] || fail "switching image session to non-vision returned $CODE, want 409"
echo "non-vision profile switch rejected"

echo "== editing a user message preserves its image attachment =="
EDIT="$(curl -sf -X POST "$BASE/sessions/$SID_IMAGE/messages/$IMAGE_MESSAGE_ID" \
    -H 'Content-Type: application/json' -d '{"content":"edited image prompt"}')"
EDIT_RUN_ID="$(printf '%s' "$EDIT" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$EDIT_RUN_ID" ] || fail "image edit did not start a run: $EDIT"
wait_run_done "$SID_IMAGE" "$EDIT_RUN_ID"
EDIT_SNAP="$(curl -sf "$BASE/sessions/$SID_IMAGE")"
printf '%s' "$EDIT_SNAP" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const user = (s.messages || []).find((m) => m.role === "user" && m.content === "edited image prompt");
        const att = user && (user.attachments || [])[0];
        if (!att || att.kind !== "image" || Object.prototype.hasOwnProperty.call(att, "content")) {
            console.error("edited image attachment was not preserved");
            process.exit(1);
        }
    });
' || fail "edited user message lost its image attachment"
echo "image attachment preserved on edit"

echo "== unsupported image mime must 400 =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID_IMAGE/runs" \
    -H 'Content-Type: application/json' \
    -d '{"session_id":"'"$SID_IMAGE"'","message":"svg","attachments":[{"name":"a.svg","kind":"image","mime_type":"image/svg+xml","content":"data:image/svg+xml;base64,PHN2Zy8+"}]}')"
[ "$CODE" = "400" ] || fail "svg image returned $CODE, want 400"
echo "400 on svg image"

echo "== unsupported kind must 400 =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID/runs" \
    -H 'Content-Type: application/json' \
    -d '{"session_id":"'"$SID"'","message":"x","attachments":[{"name":"a.bin","kind":"audio","content":"x"}]}')"
[ "$CODE" = "400" ] || fail "unsupported kind returned $CODE, want 400"
echo "400 on unsupported kind"

echo "== docx attachment: binary stored + text extracted =="
DOCX_BASE64="$("$NODE" "$ROOT/harness/tests/make_test_docx.mjs")"
[ -n "$DOCX_BASE64" ] || fail "the test docx did not build"
SID_DOCX="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_DOCX" ] || fail "no docx session id"
RUN_DOCX="$(curl -sf -X POST "$BASE/sessions/$SID_DOCX/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_DOCX\",\"message\":\"read the docx\",\"attachments\":[{\"name\":\"hello.docx\",\"kind\":\"document\",\"mime_type\":\"application/vnd.openxmlformats-officedocument.wordprocessingml.document\",\"content\":\"data:application/vnd.openxmlformats-officedocument.wordprocessingml.document;base64,$DOCX_BASE64\"}]}")"
printf '%s' "$RUN_DOCX" | grep -q '"run_id"' || fail "docx run did not start: $RUN_DOCX"
RUN_ID_DOCX="$(sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p' <<< "$RUN_DOCX")"
wait_run_done "$SID_DOCX" "$RUN_ID_DOCX"
ANSWER="$(last_answer "$SID_DOCX")"
printf '%s' "$ANSWER" | grep -q "Hello from the OOXML extraction test." \
    || fail "the docx text was not extracted into the prompt: $ANSWER"
printf '%s' "$ANSWER" | grep -q "Second paragraph." \
    || fail "the docx second paragraph was lost: $ANSWER"
printf '%s' "$ANSWER" | grep -q "attached file: hello.docx" || fail "the docx attachment note is missing"
echo "docx sheet text extracted and the workspace copy referenced"

echo "== xlsx attachment: shared strings table reads =="
SID_XLSX="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_XLSX" ] || fail "no xlsx session id"
RUN_XLSX="$(curl -sf -X POST "$BASE/sessions/$SID_XLSX/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_XLSX\",\"message\":\"read the sheet\",\"attachments\":[{\"name\":\"sheet.xlsx\",\"kind\":\"document\",\"mime_type\":\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet\",\"content\":\"data:application/vnd.openxmlformats-officedocument.spreadsheetml.sheet;base64,$DOCX_BASE64\"}]}")"
printf '%s' "$RUN_XLSX" | grep -q '"run_id"' || fail "xlsx run did not start: $RUN_XLSX"
RUN_ID_XLSX="$(sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p' <<< "$RUN_XLSX")"
wait_run_done "$SID_XLSX" "$RUN_ID_XLSX"
ANSWER_XLSX="$(last_answer "$SID_XLSX")"
printf '%s' "$ANSWER_XLSX" | grep -q "Sheet heading from the shared strings test." \
    || fail "the xlsx shared strings text was not extracted into the prompt: $ANSWER_XLSX"
echo "xlsx shared strings read"

echo "== pdf attachment: honest no-extraction note, binary serialized =="
PDF_BASE64="$(printf '%%PDF-1.4\n1 0 obj\n%% a stand-in pdf\n' | base64 -w0)"
[ -n "$PDF_BASE64" ] || fail "the pdf stub did not build"
SID_PDF="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID_PDF" ] || fail "no pdf session id"
RUN_PDF="$(curl -sf -X POST "$BASE/sessions/$SID_PDF/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID_PDF\",\"message\":\"read the pdf\",\"attachments\":[{\"name\":\"note.pdf\",\"kind\":\"document\",\"mime_type\":\"application/pdf\",\"content\":\"data:application/pdf;base64,$PDF_BASE64\"}]}")"
printf '%s' "$RUN_PDF" | grep -q '"run_id"' || fail "pdf run did not start: $RUN_PDF"
RUN_ID_PDF="$(sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p' <<< "$RUN_PDF")"
wait_run_done "$SID_PDF" "$RUN_ID_PDF"
ANSWER_PDF="$(last_answer "$SID_PDF")"
printf '%s' "$ANSWER_PDF" | grep -q "not readable in-context" \
    || fail "the pdf note is missing: $ANSWER_PDF"
printf '%s' "$ANSWER_PDF" | grep -q "attachments/" \
    || fail "the pdf workspace path is missing: $ANSWER_PDF"
if printf '%s' "$ANSWER_PDF" | grep -q "Extracted text"; then
    fail "the pdf claimed an extraction it does not do: $ANSWER_PDF"
fi
echo "pdf honestly reports no text extraction"

echo
echo "ATTACHMENTS_TEST_PASSED"
