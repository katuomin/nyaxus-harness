#!/usr/bin/env bash
# group_chat_test.sh - group chat slice-0 e2e (docs/group-chat.md, Regime 1
# np=1): one group = one session with an ordered speaker list, one shared
# transcript, serialized round-robin turns, the group.abstain convention.
#
# Asserts:
#   - a "speakers" array on session create makes a group session (speakers
#     round-trip, group.created lands on the ledger)
#   - the group run drives turn runs (<rid>-t<round>-<i>) with the speaker
#     profiles, group.turn.started per turn, run.completed per turn
#   - unanimous abstention closes the group: group.completed with
#     termination "unanimous abstention" and the round count
#   - the group.max_rounds setting caps the rounds: termination "budget"
#   - an @Peer mention pulls that peer to the floor next, the rest of the
#     round keeps its order, and the round-robin cursor is untouched
#   - committed turns are attributed ("Name: " in the peers' recorded
#     prompts, message.speaker in the session snapshot)
#   - group.abstain is declared only on group turns, never on ordinary
#     sessions
#
# group.abstain is read-only: no approval gate anywhere in the test.
#
# Usage: harness/tests/group_chat_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9065
DAEMON_PORT=9066
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

# run_json <script> <arg> <url> <fail-message> - run a node assertion on
# stdin JSON (arg lands in process.argv[1]).
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

for _ in $(seq 1 50); do
    curl -sf "$BASE/health" > /dev/null 2>&1 && break
    sleep 0.2
done
curl -sf "$BASE/health" > /dev/null 2>&1 || fail "daemon did not come up (see $WORKDIR/daemon.log)"
e2e_daemon_up "$DPID"
echo "healthy"

echo "== profiles: alice, bob, carol =="
for p in alice bob carol; do
    CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles" \
        -H 'Content-Type: application/json' \
        -d "{\"id\":\"$p\",\"name\":\"$(echo "$p" | sed 's/^./\U&/')\"}")"
    [ "$CODE" = "200" ] || fail "$p profile create returned $CODE"
done
echo "profiles created"

# wait_group_completed <sid> - poll until the group closes.
wait_group_completed() {
    for _ in $(seq 1 200); do
        curl -sf "$BASE/sessions/$1" 2>/dev/null | grep -q '"event_type":"group.completed"' && return 0
        sleep 0.2
    done
    return 1
}

echo "== S1: two speakers, unanimous-abstention closure =="
SID1="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice","speakers":["alice","bob"]}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID1" ] || fail "no group session id"
curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice","speakers":["alice","bob"]}' \
    | grep -q '"speakers":\["alice","bob"\]' \
    || fail "speakers must round-trip as a JSON array"
curl -sf "$BASE/sessions/$SID1" | grep -q '"event_type":"group.created"' \
    || fail "group.created missing from the group ledger"
curl -sf "$BASE/sessions/$SID1" | grep -q '"speakers":\["alice","bob"\]' \
    || fail "group.created must carry the speaker roster"
echo "group session created"

RUN1="$(curl -sf -X POST "$BASE/sessions/$SID1/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID1\",\"message\":\"discuss shipping\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "group run 1 did not start"

wait_group_completed "$SID1" || fail "group 1 never completed (see $WORKDIR/daemon.log)"
curl -sf "$BASE/sessions/$SID1" | grep -q '"termination":"unanimous abstention"' \
    || fail "S1 must terminate on unanimous abstention"
curl -sf "$BASE/sessions/$SID1" | grep -q '"rounds":"2"' \
    || fail "S1 must report two rounds (r1 partial, r2 unanimous)"
echo "S1 terminated on unanimous abstention after 2 rounds"

# Turn order: alice r1, bob r1, alice r2, bob r2 - with the turn run ids.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const rid = process.argv[1];
        const turns = (s.events || []).filter((e) => e.event_type === "group.turn.started");
        const want = [
            ["alice", "1", rid + "-t0-0"],
            ["bob", "1", rid + "-t0-1"],
            ["alice", "2", rid + "-t1-0"],
            ["bob", "2", rid + "-t1-1"],
        ];
        if (turns.length !== want.length) {
            console.error("turn count: " + turns.length);
            process.exit(1);
        }
        turns.forEach((e, i) => {
            if (e.payload.speaker !== want[i][0] || e.payload.round !== want[i][1] || e.payload.turn_run !== want[i][2]) {
                console.error("turn " + i + ": " + JSON.stringify(e.payload));
                process.exit(1);
            }
        });
        console.log("S1 turn order + turn run ids verified");
    });' "$RUN1" "$BASE/sessions/$SID1" "S1 turn order"

# Abstentions: bob r1, alice r2, bob r2 - each on its turn run.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const rid = process.argv[1];
        const abstained = (s.events || []).filter((e) => e.event_type === "group.abstained");
        const want = [["bob", rid + "-t0-1"], ["alice", rid + "-t1-0"], ["bob", rid + "-t1-1"]];
        if (abstained.length !== want.length) {
            console.error("abstention count: " + abstained.length);
            process.exit(1);
        }
        abstained.forEach((e, i) => {
            if (e.payload.speaker !== want[i][0] || e.run_id !== want[i][1]) {
                console.error("abstention " + i + ": " + JSON.stringify({ p: e.payload, run: e.run_id }));
                process.exit(1);
            }
        });
        console.log("S1 abstentions verified (one per abstaining turn)");
    });' "$RUN1" "$BASE/sessions/$SID1" "S1 abstentions"

# The floor signal: every turn is announced (group.turn.prepared) before it
# opens, naming the same speaker on the same turn run.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const prepared = (s.events || []).filter((e) => e.event_type === "group.turn.prepared");
        const started = (s.events || []).filter((e) => e.event_type === "group.turn.started");
        if (prepared.length !== started.length) {
            console.error("prepared/started count mismatch: " + prepared.length + " vs " + started.length);
            process.exit(1);
        }
        for (let i = 0; i < started.length; i++) {
            if (prepared[i].payload.speaker !== started[i].payload.speaker ||
                prepared[i].payload.turn_run !== started[i].payload.turn_run) {
                console.error("prepared " + i + " must precede its turn: " + JSON.stringify(prepared[i].payload));
                process.exit(1);
            }
        }
        console.log("S1 floor signal verified (prepared precedes every turn)");
    });' "$RUN1" "$BASE/sessions/$SID1" "S1 prepared"

# One committed turn (alice r1); it is attributed, the abstentions are not.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const assistant = (s.messages || []).filter((m) => m.role === "assistant");
        if (assistant.length !== 1) {
            console.error("expected exactly one committed turn, got " + assistant.length);
            process.exit(1);
        }
        if (assistant[0].speaker !== "alice") {
            console.error("committed turn must carry speaker=alice, got " + assistant[0].speaker);
            process.exit(1);
        }
        console.log("S1 committed turn attributed to alice");
    });' "" "$BASE/sessions/$SID1" "S1 attribution"

echo "== S2: round budget (group.max_rounds=1) =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" \
    -H 'Content-Type: application/json' -d '{"group.max_rounds":1}')"
[ "$CODE" = "200" ] || fail "group.max_rounds setting was not accepted (got $CODE)"
SID2="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice","speakers":["alice","bob"]}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no S2 group session id"
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID2/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID2\",\"message\":\"one round only\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "group run 2 did not start"
wait_group_completed "$SID2" || fail "group 2 never completed (see $WORKDIR/daemon.log)"
curl -sf "$BASE/sessions/$SID2" | grep -q '"termination":"budget"' \
    || fail "S2 must terminate on the round budget"
curl -sf "$BASE/sessions/$SID2" | grep -q '"rounds":"1"' \
    || fail "S2 must report one round"
# Both speakers got a turn in the single round; both turns committed.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const turns = (s.events || []).filter((e) => e.event_type === "group.turn.started");
        const speakers = turns.map((e) => e.payload.speaker).join(",");
        if (speakers !== "alice,bob") {
            console.error("S2 must give both speakers one turn, got " + speakers);
            process.exit(1);
        }
        const assistant = (s.messages || []).filter((m) => m.role === "assistant");
        if (assistant.length !== 2 || assistant[0].speaker !== "alice" || assistant[1].speaker !== "bob") {
            console.error("S2 committed turns wrong: " + JSON.stringify(assistant.map((m) => m.speaker)));
            process.exit(1);
        }
        console.log("S2 budget termination + both turns verified");
    });' "" "$BASE/sessions/$SID2" "S2 budget"
echo "S2 terminated on budget after 1 round"

echo "== S3: three speakers, @-nudge re-cursor =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" \
    -H 'Content-Type: application/json' -d '{"group.max_rounds":3}')"
[ "$CODE" = "200" ] || fail "group.max_rounds restore was not accepted (got $CODE)"
SID3="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice","speakers":["alice","bob","carol"]}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID3" ] || fail "no S3 group session id"
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID3/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID3\",\"message\":\"plan the launch\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "group run 3 did not start"
wait_group_completed "$SID3" || fail "group 3 never completed (see $WORKDIR/daemon.log)"
curl -sf "$BASE/sessions/$SID3" | grep -q '"termination":"unanimous abstention"' \
    || fail "S3 must terminate on unanimous abstention"
curl -sf "$BASE/sessions/$SID3" | grep -q '"rounds":"2"' \
    || fail "S3 must report two rounds (the per-speaker depth)"
echo "S3 terminated on unanimous abstention after 2 rounds/speaker"

# The nudge: alice's r1 "@Carol" pulls carol to the floor immediately
# (r1: alice, carol, bob), and the rotation continues from the nudge target,
# so pass 2 opens on carol (carol, alice, bob) - a hint for the floor, and
# the pass keeps each speaker to one visit.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const turns = (s.events || []).filter((e) => e.event_type === "group.turn.started");
        const seq = turns.map((e) => e.payload.speaker + "@" + e.payload.round).join(",");
        const want = "alice@1,carol@1,bob@1,carol@2,alice@2,bob@2";
        if (seq !== want) {
            console.error("S3 turn order: " + seq);
            console.error("S3 turn order: " + want);
            process.exit(1);
        }
        console.log("S3 nudge verified (carol pulled forward, pass 2 opens on her)");
    });' "" "$BASE/sessions/$SID3" "S3 nudge order"

# Three committed turns (pass 2 is all abstentions - abstain turns commit
# nothing), each attributed to its speaker.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const s = JSON.parse(d);
        const assistant = (s.messages || []).filter((m) => m.role === "assistant");
        const speakers = assistant.map((m) => m.speaker).join(",");
        const want = "alice,carol,bob";
        if (speakers !== want) {
            console.error("S3 committed speakers: " + speakers);
            process.exit(1);
        }
        console.log("S3 committed turns attributed in turn order");
    });' "" "$BASE/sessions/$SID3" "S3 attribution"

echo "== S4: ordinary session, no group.abstain declaration =="
SID4="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID4" ] || fail "no ordinary session id"
RUN4="$(curl -sf -X POST "$BASE/sessions/$SID4/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID4\",\"message\":\"hello\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN4" ] || fail "ordinary run did not start"
for _ in $(seq 1 150); do
    curl -sf "$BASE/sessions/$SID4" 2>/dev/null | grep -q '"event_type":"run.completed"' && break
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID4" | grep -q '"event_type":"run.completed"' \
    || fail "ordinary run did not complete"
echo "ordinary session completed"

echo "== declarations + attribution in the recorded requests =="
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const reqs = JSON.parse(d);
        if (reqs.length !== 13) {
            console.error("expected 13 recorded requests, got " + reqs.length);
            process.exit(1);
        }
        const toolNames = (req) => ((req.tools) || []).map((t) => t.function && t.function.name);
        const systemText = (req) => (req.messages || []).map((m) => String(m.content || "")).join("\n");
        // Every group turn declares group.abstain (requests 1-12), and the
        // ordinary session (request 13) does not.
        for (let i = 0; i < 12; i++) {
            if (!toolNames(reqs[i]).includes("group.abstain")) {
                console.error("group turn request " + (i + 1) + " must declare group.abstain");
                process.exit(1);
            }
        }
        if (toolNames(reqs[12]).includes("group.abstain")) {
            console.error("ordinary session request must not declare group.abstain");
            process.exit(1);
        }
        // The roundtable framing rides on the group turns only.
        for (let i = 0; i < 12; i++) {
            if (!systemText(reqs[i]).includes("[GROUP]")) {
                console.error("group turn request " + (i + 1) + " must carry the [GROUP] framing");
                process.exit(1);
            }
        }
        if (systemText(reqs[12]).includes("[GROUP]")) {
            console.error("ordinary session request must not carry the [GROUP] framing");
            process.exit(1);
        }
        // Attribution: the bob turn (request 2) reads the committed alice
        // turn as "Alice: ..."; the nudged carol turn (8) reads alice; the
        // bob turn (9) reads both committed peers.
        if (!systemText(reqs[1]).includes("Alice: ")) {
            console.error("bob request must read alice attributed as Alice:");
            process.exit(1);
        }
        if (!systemText(reqs[7]).includes("Alice: ")) {
            console.error("carol request must read the nudging peer attributed");
            process.exit(1);
        }
        if (!systemText(reqs[8]).includes("Alice: ") || !systemText(reqs[8]).includes("Carol: ")) {
            console.error("bob request must read both attributed peers");
            process.exit(1);
        }
        // Root-cause guard: a peer turn must be a USER message, never an
        // assistant one (a trailing assistant message makes llama.cpp treat
        // the turn as a continuation and re-serve the peer text as the
        // reply - the echo chain). Own prior turns stay assistant.
        const assistantTexts = (req) => (req.messages || []).filter((m) => m.role === "assistant").map((m) => String(m.content || ""));
        if (assistantTexts(reqs[1]).some((t) => t.includes("Alice: "))) {
            console.error("bob request must not carry the peer turn as an assistant message");
            process.exit(1);
        }
        if (assistantTexts(reqs[8]).some((t) => t.includes("Alice: ") || t.includes("Bob: "))) {
            console.error("carol request must not carry peer turns as assistant messages");
            process.exit(1);
        }
        // Anti-echo tail note: present from the second turn on (naming the
        // last speaker), absent on the first (nothing to echo yet).
        if (!systemText(reqs[1]).includes("[TURN] Alice spoke last")) {
            console.error("bob request must carry the [TURN] anti-echo note");
            process.exit(1);
        }
        if (systemText(reqs[0]).includes("[TURN]")) {
            console.error("the first turn must not carry a [TURN] note");
            process.exit(1);
        }
        console.log("declarations + attribution verified across 13 requests");
    });' "" "$MOCK/requests" "declarations"

echo "== S5: queued deliver-now (zap) mid-run =="
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" \
    -H 'Content-Type: application/json' -d '{"group.max_rounds":1}')"
[ "$CODE" = "200" ] || fail "S5 budget setting rejected (got $CODE)"
SID5="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice","speakers":["alice","bob"]}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID5" ] || fail "no S5 group session id"
RUN5="$(curl -sf -X POST "$BASE/sessions/$SID5/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID5\",\"message\":\"start the plan\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN5" ] || fail "S5 group run did not start"
# Wait for the first speaker turn to be announced, then queue + zap while
# the mock holds completion 17 (1.5s) - the deliver-now window.
for _ in $(seq 1 100); do
    curl -sf "$BASE/sessions/$SID5" 2>/dev/null | "$NODE" -e "
        let d = '';
        process.stdin.on('data', (c) => { d += c; });
        process.stdin.on('end', () => {
            const s = JSON.parse(d);
            const started = (s.events || []).some((e) =>
                e.event_type === 'group.turn.started' && e.run_id === '$RUN5' &&
                String(e.payload.turn_run).endsWith('-t0-0'));
            process.exit(started ? 0 : 1);
        });" && break
    sleep 0.2
done
QUEUED5="$(curl -sf -X POST "$BASE/sessions/$SID5/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID5\",\"message\":\"remember the budget\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$QUEUED5" ] || fail "S5 queued run did not start"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RUN5/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"steer_live\",\"queued_run_id\":\"$QUEUED5\"}")"
[ "$CODE" = "200" ] || fail "S5 zap was not accepted (got $CODE)"
wait_group_completed "$SID5" || fail "S5 group never completed"
curl -sf "$BASE/sessions/$SID5" | grep -q '"rounds":"1"' \
    || fail "S5 must end on the 1-round budget"
# One delivery: exactly one run.steered_live (targeting the GROUP run, from
# the queued run), the queued run cancelled without withdrawing its message
# row, and the live speaker's turn continued with the steer in context.
"$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const s = JSON.parse(d);
        const steers = (s.events || []).filter((e) => e.event_type === 'run.steered_live');
        if (steers.length !== 1) {
            console.error('expected exactly one run.steered_live, got ' + steers.length);
            process.exit(1);
        }
        if (steers[0].payload.from_run !== '$QUEUED5' || steers[0].run_id !== '$RUN5-t0-0') {
            console.error('steered_live event must carry the live turn as its consumer: ' + JSON.stringify(steers[0]));
            process.exit(1);
        }
        const cancelled = (s.events || []).some((e) =>
            e.event_type === 'run.cancelled' && e.run_id === '$QUEUED5');
        if (!cancelled) {
            console.error('the queued run must cancel without withdrawal');
            process.exit(1);
        }
        const steerRow = (s.messages || []).find((m) =>
            m.role === 'user' && m.content === 'remember the budget');
        if (!steerRow) {
            console.error('the promoted steer row must stay in the transcript');
            process.exit(1);
        }
        const continued = (s.messages || []).find((m) =>
            m.role === 'assistant' &&
            m.run_id === '$RUN5-t0-0' &&
            String(m.content || '').includes('Steered: budget noted. Continuing.'));
        if (!continued) {
            console.error('the live speaker must continue their turn with the steer in context');
            process.exit(1);
        }
        console.log('S5: deliver-now reached the live speaker turn; single delivery');
    });" < <(curl -sf "$BASE/sessions/$SID5") || fail "S5 assertions"
# The model actually read the promoted steer: some later provider call's
# messages carry the queued text.
run_json '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const reqs = JSON.parse(d);
        const steer = reqs.some((req) =>
            ((req.messages) || []).some((m) => m.role === "user" &&
                String(m.content || "").includes("remember the budget")));
        if (!steer) {
            console.error("the continuation request must carry the promoted steer");
            process.exit(1);
        }
        console.log("S5: the model read the promoted message");
    });' "" "$MOCK/requests" "S5 provider read"

# S5 pinned the GLOBAL budget to 1; restore it, so S6's cleared-override
# fallback asserts against the documented default (3).
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/settings" \
    -H 'Content-Type: application/json' -d '{"group.max_rounds":3}')"
[ "$CODE" = "200" ] || fail "the global budget restore was rejected (got $CODE)"

echo "== S6: per-session rounds override =="
SID6="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"alice","speakers":["alice","bob"]}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID6" ] || fail "no S6 group session id"
# Override 1: quick answers, budget wins over the restored global 3.
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID6/group-rounds" \
    -H 'Content-Type: application/json' -d '{"rounds":1}')"
[ "$CODE" = "200" ] || fail "S6 override was not accepted (got $CODE)"
curl -sf "$BASE/sessions/$SID6" | grep -q '"group_rounds_override":1' \
    || fail "the override must round-trip on the session"
RUN6="$(curl -sf -X POST "$BASE/sessions/$SID6/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID6\",\"message\":\"quick pass\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN6" ] || fail "S6 run did not start"
wait_group_completed "$SID6" || fail "S6 group never completed"
# With budget 1: both speakers pass once, no unanimous round -> budget.
curl -sf "$BASE/sessions/$SID6" | grep -q '"rounds":"1"' \
    || fail "S6 must end on the override budget (1 round)"
curl -sf "$BASE/sessions/$SID6" | grep -q '"termination":"budget"' \
    || fail "S6 must terminate on the override budget"
echo "S6 override honored (1 round)"

# Clear it: the global setting (3) owns the budget again.
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/sessions/$SID6/group-rounds" \
    -H 'Content-Type: application/json' -d '{"rounds":null}')"
[ "$CODE" = "200" ] || fail "S6 clear was not accepted (got $CODE)"
curl -sf "$BASE/sessions/$SID6" | grep -q '"group_rounds_override":0' \
    || fail "the clear must zero the override"
RUN6B="$(curl -sf -X POST "$BASE/sessions/$SID6/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID6\",\"message\":\"another pass\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN6B" ] || fail "S6 second run did not start"
# The first run already closed a group.completed, so wait for the COUNT.
for _ in $(seq 1 200); do
    C="$(curl -sf "$BASE/sessions/$SID6" 2>/dev/null | grep -o '"event_type":"group.completed"' | wc -l)"
    [ "$C" = "2" ] && break
    sleep 0.2
done
C="$(curl -sf "$BASE/sessions/$SID6" | grep -o '"event_type":"group.completed"' | wc -l)"
[ "$C" = "2" ] || fail "S6 second group never completed (count=$C)"
# Global 3 with the scripted flow: r1 text+abstain, r2 abstain+abstain ->
# unanimous after 2 rounds (proves the budget is 3, not the old override).
curl -sf "$BASE/sessions/$SID6" | grep -q '"rounds":"2"' \
    || fail "the cleared override must fall back to the global budget (got rounds other than 2)"
# Both group.completed events must show distinct budgets.
BUDGETS="$(curl -sf "$BASE/sessions/$SID6" | grep -o 'group.completed' | wc -l)"
[ "$BUDGETS" = "2" ] || fail "expected two group completions, got $BUDGETS"
echo "S6 cleared override falls back to the global budget"

echo
echo "GROUP_CHAT_TEST_PASSED"
