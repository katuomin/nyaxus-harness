#!/usr/bin/env bash
# subagents_test.sh - agent.spawn (subagents slice-0, docs/subagents.md):
# the profile-scoped spawn tool, the objective-only child session, the
# worker.* ledger events, and the harvested bounded narrative.
#
# Asserts:
#   - agent.spawn is declared for a profile with a non-empty
#     child_profile_allowlist (and the child_profile_allowlist field round-
#     trips the profile API as a JSON array)
#   - a spawn call creates a child session (parent_session_id link, worker
#     profile, objective-only seed) and runs it to completion
#   - the parent's tool result carries the child's final narrative, and
#     worker.started + worker.completed land on the parent's ledger
#   - a spawn outside the allowlist is an in-band tool error, not a crash
#   - the worker's prompt is objective-only: no [CORE MEMORY], no
#     [PREV SESSION] (a seeded prior worker session proves the gate), no
#     [Memory:], and no memory tools declared
#
# agent.spawn is not read-only, so each spawn opens the ordinary approval
# gate (design: children run the ordinary pipeline); the test approves both
# gates with scope=once.
#
# Usage: harness/tests/subagents_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9045
DAEMON_PORT=9044
EMBED_MODEL="$ROOT/harness/models/nyx/qwen3-embedding-0.6b-int8.nyx"
[ -f "$EMBED_MODEL" ] || { echo "embedding model not found: $EMBED_MODEL" >&2; exit 1; }
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"
MOCK="http://127.0.0.1:$MOCK_PORT"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

# latest_approval_id <sid> -> the newest pending approval.requested id
latest_approval_id() {
    curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const s = JSON.parse(d);
            const reqs = (s.events || []).filter((e) => e.event_type === "approval.requested");
            process.stdout.write(reqs.length ? String(reqs[reqs.length - 1].payload.approval_id || "") : "");
        });'
}

approve_gate() { # approve_gate <run_id> <new_approval_id>
    local code
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$1/commands" \
        -H 'Content-Type: application/json' \
        -d "{\"command\":\"approve\",\"approval_id\":\"$2\",\"scope\":\"once\"}")"
    [ "$code" = "200" ] || fail "spawn approval was not accepted (got $code)"
}

e2e_port_free "$DAEMON_PORT"
e2e_port_free "$MOCK_PORT"

echo "== launch mock provider + daemon =="
MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_subagents_provider.mjs" > mock.log 2>&1 &
MPID=$!
# Memory is on (custom bank) so the core-unit and memory-tool gates have
# something to trim; retain is disabled so the mock never sees memory calls.
( cd "$WORKDIR" && NYAXUS_MEMORY_ENABLED=1 \
    NYAXUS_MEMORY_PROVIDER=custom \
    NYAXUS_MEMORY_DB_PATH="$WORKDIR/memory.db" \
    NYAXUS_MEMORY_EMBED_MODEL="$EMBED_MODEL" \
    NYAXUS_MEMORY_RETAIN_EVERY_N_TURNS=1000000 \
    NYAXUS_MEMORY_SWEEP_IDLE_SECONDS=3600 \
    exec "$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$MOCK_PORT/v1" --model mock-model ) > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$MPID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null 2>&1 || fail "daemon did not come up (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "healthy"

echo "== profiles: worker + boss (allowlist) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles" \
    -H 'Content-Type: application/json' -d '{"id":"worker","name":"worker"}')"
[ "$CODE" = "200" ] || fail "worker profile create returned $CODE"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles" \
    -H 'Content-Type: application/json' \
    -d '{"id":"boss","name":"boss","child_profile_allowlist":["worker"]}')"
[ "$CODE" = "200" ] || fail "boss profile create returned $CODE"
curl -sf "$BASE/profiles" | grep -q '"child_profile_allowlist":\["worker"\]' \
    || fail "child_profile_allowlist must round-trip as a JSON array"
echo "profiles created"

echo "== memory: core unit pinned + worker prior session seeded =="
RECORD_ID="$(curl -sf -X POST "$BASE/memory/records" -H 'Content-Type: application/json' \
    -d '{"text":"the operator prefers concise answers"}' \
    | sed -n 's/.*"id":\([0-9]*\).*/\1/p')"
[ -n "$RECORD_ID" ] || fail "core unit record did not create"
curl -sf -X POST "$BASE/memory/records/$RECORD_ID/approve" > /dev/null \
    || fail "core unit approve failed"
# Approval materializes the unit via the durable retain outbox (async):
# poll the bank until the unit row exists, then pin it as core.
UNIT_ID=""
for _ in $(seq 1 150); do
    UNIT_ID="$(curl -sf "$BASE/memory/bank" 2>/dev/null | "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            try {
                const unit = (JSON.parse(d).units || [])[0];
                if (unit && unit.id) console.log(String(unit.id).replace("mem:", ""));
            } catch {}
        });')"
    [ -n "$UNIT_ID" ] && break
    sleep 0.2
done
[ -n "$UNIT_ID" ] || fail "core unit never materialized (see $WORKDIR/daemon.log)"
curl -sf -X POST "$BASE/memory/units/$UNIT_ID/core" > /dev/null \
    || fail "core unit pin failed"
WORKER_PRIOR="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"worker"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$WORKER_PRIOR" ] || fail "worker prior session did not create"
echo "core unit + worker prior session seeded"

echo "== parent run 1: spawn a worker =="
SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"boss"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no parent session id"

RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"spawn a worker to count to three\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "parent run 1 did not start"

APPROVAL1=""
for _ in $(seq 1 75); do
    APPROVAL1="$(latest_approval_id "$SID")"
    [ -n "$APPROVAL1" ] && break
    sleep 0.2
done
[ -n "$APPROVAL1" ] || fail "the spawn never opened a gate (see $WORKDIR/daemon.log)"
approve_gate "$RUN1" "$APPROVAL1"
echo "spawn gate approved"

for _ in $(seq 1 150); do
    curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"' && break
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"run.completed"' \
    || fail "parent run 1 did not complete"
echo "parent run 1 completed"

echo "== child session: spawned, linked, objective-only =="
CHILD_ID="$(curl -sf "$BASE/sessions" | "$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const sessions = JSON.parse(d).sessions;
        const child = sessions.find((s) => s.parent_session_id === '"$SID"');
        if (child) console.log(child.id);
    });")"
[ -n "$CHILD_ID" ] || fail "no child session with parent_session_id=$SID"
curl -sf "$BASE/sessions/$CHILD_ID" | grep -q '"profile":"worker"' \
    || fail "child session must be bound to the worker profile"
curl -sf "$BASE/sessions/$CHILD_ID" | grep -q '"title":"worker: task 1"' \
    || fail "child session title must carry the objective"
curl -sf "$BASE/sessions/$CHILD_ID" | grep -q 'Task: task 1' \
    || fail "child transcript must be seeded with the objective"
curl -sf "$BASE/sessions/$CHILD_ID" | grep -q '"event_type":"run.completed"' \
    || fail "child run did not complete"
echo "child session verified"

echo "== harvest: narrative + worker.* events on the parent ledger =="
curl -sf "$BASE/sessions/$SID" | grep -q "Worker report: counted 1, 2, 3." \
    || fail "parent must harvest the child's final narrative"
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"worker.started"' \
    || fail "worker.started missing from the parent ledger"
curl -sf "$BASE/sessions/$SID" | grep -q '"event_type":"worker.completed"' \
    || fail "worker.completed missing from the parent ledger"
curl -sf "$BASE/sessions/$SID" | grep -q '"child_session_id":"'"$CHILD_ID"'"' \
    || fail "worker events must carry the child session id"
echo "harvest verified"

echo "== parent run 2: spawn outside the allowlist is an in-band error =="
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"spawn something not allowed\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "parent run 2 did not start"

APPROVAL2=""
for _ in $(seq 1 75); do
    APPROVAL2="$(latest_approval_id "$SID")"
    [ "$APPROVAL2" != "$APPROVAL1" ] && [ -n "$APPROVAL2" ] && break
    sleep 0.2
done
[ -n "$APPROVAL2" ] && [ "$APPROVAL2" != "$APPROVAL1" ] \
    || fail "the disallowed spawn never opened a new gate"
approve_gate "$RUN2" "$APPROVAL2"
echo "second gate approved"

for _ in $(seq 1 150); do
    COUNT="$(curl -sf "$BASE/sessions/$SID" | grep -c '"event_type":"run.completed"')"
    [ "$COUNT" = "2" ] && break
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID" | grep -q "not in the allowlist" \
    || fail "allowlist miss must surface as an in-band tool error"
echo "allowlist miss verified"

echo "== declaration: agent.spawn only on the allowlisted profile =="
"$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const reqs = JSON.parse(d);
        const names = ((reqs[0] && reqs[0].tools) || []).map((t) => t.function && t.function.name);
        if (!names.includes('agent.spawn')) {
            console.error('request 1 (boss profile) must declare agent.spawn');
            process.exit(1);
        }
        console.log('agent.spawn declared for the allowlisted profile');
    });" < <(curl -sf "$MOCK/requests") || fail "agent.spawn declaration"

echo "== prompt trim: worker is objective-only (no memory, no prev session) =="
"$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const reqs = JSON.parse(d);
        const systemText = (req) => (req.messages || []).map((m) => String(m.content || '')).join('\n');
        const toolNames = (req) => ((req.tools) || []).map((t) => t.function && t.function.name);
        const boss = reqs[0];
        const worker = reqs[1];
        if (!systemText(boss).includes('[CORE MEMORY]')) {
            console.error('boss request must carry the pinned [CORE MEMORY] block');
            process.exit(1);
        }
        if (!toolNames(boss).includes('memory.recall')) {
            console.error('boss request must declare memory.recall');
            process.exit(1);
        }
        const workerText = systemText(worker);
        for (const marker of ['[CORE MEMORY]', '[PREV SESSION]', '[Memory:']) {
            if (workerText.includes(marker)) {
                console.error('worker request must not carry ' + marker);
                process.exit(1);
            }
        }
        if (toolNames(worker).includes('memory.recall')) {
            console.error('worker request must not declare memory.recall');
            process.exit(1);
        }
        console.log('worker prompt trimmed: no core memory, no prev session, no memory tools');
    });" < <(curl -sf "$MOCK/requests") || fail "worker prompt trim"

echo
echo "SUBAGENTS_TEST_PASSED"
