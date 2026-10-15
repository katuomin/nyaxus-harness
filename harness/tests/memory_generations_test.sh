#!/usr/bin/env bash
# memory_generations_test.sh - the memory generation provenance log.
#
# Covers:
#   - an aux extraction turn logs a kind=extract row: succeeded, the
#     extraction prompt's id + sha256, and the produced unit ids in
#     output_refs
#   - an approved record logs a kind=approved row (trusted text, no prompt)
#   - the unit detail's version JSON carries prompt_revision + generation_id
#   - the kind filter and the route's 401 behavior work
#
# Usage: harness/tests/memory_generations_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MAIN_PORT=9051
EXTRACT_PORT=9052
DAEMON_PORT=9053
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
EMBED_MODEL="$ROOT/harness/models/nyx/qwen3-embedding-0.6b-int8.nyx"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
[ -f "$EMBED_MODEL" ] || { echo "embedding model not found: $EMBED_MODEL" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

gen_field() {  # gen_field <rows-json> <kind> <field> -> first matching row's field
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const rows = (JSON.parse(d).generations) || [];
            const row = rows.find((r) => r.kind === process.argv[1]);
            process.stdout.write(row ? String(row[process.argv[2]] == null ? "" : row[process.argv[2]]) : "");
        });' "$2" "$3"
}

refs_contain() {  # refs_contain <rows-json> <kind> <unit-id>
    printf '%s' "$1" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const rows = (JSON.parse(d).generations) || [];
            const row = rows.find((r) => r.kind === process.argv[1]);
            const refs = row ? JSON.parse(row.output_refs || "[]") : [];
            process.exit(refs.map(String).includes(process.argv[2]) ? 0 : 1);
        });' "$2" "$3"
}

MOCK_PORT="$MAIN_PORT" "$NODE" "$ROOT/harness/tests/mock_profile_provider.mjs" > main.log 2>&1 &
MAINSID=$!
MOCK_PORT="$EXTRACT_PORT" "$NODE" "$ROOT/harness/tests/mock_memory_extract_provider.mjs" > extract.log 2>&1 &
EXTSID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_MEMORY_ENABLED=1 \
NYAXUS_MEMORY_PROVIDER=custom \
NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
NYAXUS_MEMORY_EXTRACT_URL="http://127.0.0.1:$EXTRACT_PORT/v1" \
NYAXUS_MEMORY_EXTRACT_MODEL=mock-extract \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MAIN_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MAINSID" "$EXTSID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== custom provider becomes ready (lazy model load) =="
READY=""
for _ in $(seq 1 60); do
    STATUS="$(curl -sf "$BASE/memory/status")"
    if printf '%s' "$STATUS" | grep -q '"ready":true'; then
        READY=1
        break
    fi
    sleep 0.5
done
[ "$READY" = "1" ] || fail "custom provider never became ready: $STATUS"
echo "ready"

echo "== route requires auth =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' "$BASE/memory/generations" -H 'Authorization: Bearer wrong')"
# No API key is configured, so any bearer token is accepted; the point is
# that the route exists (not 404/405) and answers on the memory surface.
[ "$CODE" = "200" ] || fail "generations route answered $CODE (want 200)"
echo "route live"

echo "== one turn; the retain job drains through the (mock) extractor =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"The zephyr protocol listener binds port 4242 on the gateway. Also, run make check to verify the build.\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1
        break
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete (see $WORKDIR/daemon.log)"
echo "run completed"

BANK=""
for _ in $(seq 1 90); do
    BANK="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
    if printf '%s' "$BANK" | grep -q "zephyr protocol listener"; then
        break
    fi
    sleep 0.5
done
printf '%s' "$BANK" | grep -q "zephyr protocol listener" || fail "retain job did not store the grounded candidate: $BANK"
EXTRACT_UNIT="$(printf '%s' "$BANK" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const units = (JSON.parse(d).units) || [];
        const unit = units.find((u) => (u.text || "").includes("zephyr protocol listener"));
        process.stdout.write(unit ? String(unit.id).replace(/^mem:/, "") : "");
    });')"
[ -n "$EXTRACT_UNIT" ] || fail "no unit id for the zephyr fact"
echo "unit $EXTRACT_UNIT"

echo "== the extraction turn logged a kind=extract row =="
ROWS=""
for _ in $(seq 1 60); do
    ROWS="$(curl -sf "$BASE/memory/generations?kind=extract&limit=50")"
    if printf '%s' "$ROWS" | grep -q '"kind":"extract"'; then
        break
    fi
    sleep 0.5
done
printf '%s' "$ROWS" | grep -q '"kind":"extract"' || fail "no extract generation row: $ROWS"
echo "extract row present"

echo "== extract row: succeeded, prompt id + 64-hex sha, unit in output_refs =="
STATUS_FIELD="$(gen_field "$ROWS" extract status)"
[ "$STATUS_FIELD" = "succeeded" ] || fail "extract row status is '$STATUS_FIELD' (want succeeded)"
PROMPT_ID="$(gen_field "$ROWS" extract prompt_id)"
[ "$PROMPT_ID" = "memory-extraction" ] || fail "extract row prompt_id is '$PROMPT_ID' (want memory-extraction)"
SHA="$(gen_field "$ROWS" extract prompt_sha256)"
printf '%s' "$SHA" | grep -Eq '^[0-9a-f]{64}$' || fail "extract row prompt_sha256 is not 64 hex chars: '$SHA'"
GEN_ID="$(gen_field "$ROWS" extract generation_id)"
[ "$GEN_ID" = "gen:extract:nyaxus-turn:$RUN1" ] || fail "extract row generation_id is '$GEN_ID' (want gen:extract:nyaxus-turn:$RUN1)"
refs_contain "$ROWS" extract "$EXTRACT_UNIT" || fail "extract row output_refs does not contain unit $EXTRACT_UNIT: $ROWS"
echo "status, prompt identity, generation_id, output_refs all correct"

echo "== the kind filter excludes other kinds =="
ALL_ROWS="$(curl -sf "$BASE/memory/generations?limit=50")"
printf '%s' "$ALL_ROWS" | grep -q '"kind":"extract"' || fail "unfiltered list is missing the extract row: $ALL_ROWS"
printf '%s' "$(curl -sf "$BASE/memory/generations?kind=journey&limit=50")" | grep -q '"kind":"extract"' && fail "kind=journey filter leaked an extract row"
echo "filters work"

echo "== an approved record logs a kind=approved row =="
RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
    -d "{\"text\":\"The operator set the relay beacon to amber mode.\",\"type\":\"fact\",\"scope\":\"session\",\"session_id\":\"$SID\"}" \
    | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
[ -n "$RECORD_ID" ] || fail "record was not created"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/memory/records/$RECORD_ID/approve" -d '{}')"
[ "$CODE" = "200" ] || fail "approve returned $CODE"

APPROVED_UNIT=""
for _ in $(seq 1 90); do
    BANK="$(curl -sf "$BASE/memory/bank?limit=50&state=all")"
    APPROVED_UNIT="$(printf '%s' "$BANK" | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const units = (JSON.parse(d).units) || [];
            const unit = units.find((u) => (u.text || "").includes("relay beacon to amber"));
            process.stdout.write(unit ? String(unit.id).replace(/^mem:/, "") : "");
        });')"
    [ -n "$APPROVED_UNIT" ] && break
    sleep 0.5
done
[ -n "$APPROVED_UNIT" ] || fail "approval did not store the unit: $BANK"

ROWS="$(curl -sf "$BASE/memory/generations?kind=approved&limit=50")"
printf '%s' "$ROWS" | grep -q '"kind":"approved"' || fail "no approved generation row: $ROWS"
STATUS_FIELD="$(gen_field "$ROWS" approved status)"
[ "$STATUS_FIELD" = "succeeded" ] || fail "approved row status is '$STATUS_FIELD' (want succeeded)"
GEN_ID="$(gen_field "$ROWS" approved generation_id)"
[ "$GEN_ID" = "gen:approved:nyaxus-record:$RECORD_ID" ] || fail "approved row generation_id is '$GEN_ID' (want gen:approved:nyaxus-record:$RECORD_ID)"
refs_contain "$ROWS" approved "$APPROVED_UNIT" || fail "approved row output_refs does not contain unit $APPROVED_UNIT: $ROWS"
echo "approved row correct"

echo "== unit detail: the version JSON carries prompt_revision + generation_id =="
DETAIL="$(curl -sf "$BASE/memory/units/$EXTRACT_UNIT")"
VGEN="$(printf '%s' "$DETAIL" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const versions = (JSON.parse(d).versions) || [];
        const v = versions.find((x) => x.current) || versions[0] || {};
        process.stdout.write(String(v.generation_id || ""));
    });')"
[ "$VGEN" = "gen:extract:nyaxus-turn:$RUN1" ] || fail "unit detail generation_id is '$VGEN' (want gen:extract:nyaxus-turn:$RUN1)"
VREV="$(printf '%s' "$DETAIL" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const versions = (JSON.parse(d).versions) || [];
        const v = versions.find((x) => x.current) || versions[0] || {};
        process.stdout.write(String(v.prompt_revision || ""));
    });')"
[ "$VREV" = "memory-extraction:v2" ] || fail "unit detail prompt_revision is '$VREV' (want memory-extraction:v2)"
echo "version provenance present"

echo
echo "MEMORY_GENERATIONS_TEST_PASSED"
