#!/usr/bin/env bash
# sudo_password_test.sh - the sudo password gate e2e (docs/sudo-gate.md):
# a headless sudo parks the run on an input gate, the operator's answer
# (password + scope) retries the command through an askpass grant, and the
# password never appears in the transcript, the ledger, or argv.
#
# Uses a fake sudo on PATH (mirror of the real askpass contract: -A runs
# SUDO_ASKPASS and compares; without -A it fails exactly like headless
# sudo: "A terminal is required to authenticate") and a scripted mock
# provider whose closing text echoes the expected tool result.
#
# Scenarios:
#   run 1: sudo whoami - gate, answered once → works; the pwfile pair is
#          cleaned up (once scope leaves nothing on /dev/shm)
#   run 2: sudo id    - gate, answered with a WRONG password → sudo rejects,
#          the result carries the hint, nothing is cached
#   run 3: sudo whoami - gate, answered for 15 minutes (ttl) → works, the
#          grant persists
#   run 4: sudo ls    - the ttl grant covers it: no gate at all
#   run 5: sudo ps    - fake sudo in the no-new-privileges mode: the hint
#          surfaces in-band and no gate opens (a password cannot lift it)
# Throughout: neither password string appears anywhere in the session ledger.
#
# Usage: harness/tests/sudo_password_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"
MOCK_PORT=9075
DAEMON_PORT=9076
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

e2e_port_free "$DAEMON_PORT"
e2e_port_free "$MOCK_PORT"

echo "== fake sudo + mock provider + daemon =="
mkdir -p bin
cat > bin/sudo <<'FAKE'
#!/bin/sh
# Fake sudo: the real askpass contract. -A executes SUDO_ASKPASS (the helper
# prints the password) and compares; any other shape fails like headless
# sudo's TTY check does. NYAXUS_FAKE_SUDO_MODE_FILE=nnp mode mirrors the
# NoNewPrivileges refusal, which no password can lift.
mode="$(cat "$NYAXUS_FAKE_SUDO_MODE_FILE" 2>/dev/null || echo tty)"
if [ "$mode" = "nnp" ]; then
    echo 'sudo: The "no new privileges" flag is set, which prevents sudo from running as root.' >&2
    exit 1
fi
if [ "$1" = "-A" ]; then
    shift
    pw="$("$SUDO_ASKPASS")" || exit 126
    if [ "$pw" != "$NYAXUS_FAKE_SUDO_PW" ]; then
        echo "sudo: 3 incorrect password attempts" >&2
        exit 1
    fi
    echo "sudo-ok $*"
    exit 0
fi
echo "sudo: A terminal is required to authenticate" >&2
exit 1
FAKE
chmod +x bin/sudo
echo tty > fake-sudo-mode

# Stale grants from interrupted previous runs would defeat the cleanup
# assertions: sweep both possible grant bases (the /dev/shm preference and
# the platform temp fallback) before the daemon comes up.
"$NODE" -e "
    const fs = require('fs');
    const os = require('os');
    const path = require('path');
    const dirs = ['/dev/shm', os.tmpdir()];
    if (process.env.LOCALAPPDATA) dirs.push(path.join(process.env.LOCALAPPDATA, 'Temp'));
    for (const dir of [...new Set(dirs)]) {
        try {
            for (const file of fs.readdirSync(dir)) {
                if (file.startsWith('nyaxus-sudo-')) {
                    try { fs.unlinkSync(dir + '/' + file); } catch {}
                }
            }
        } catch {}
    }"

MOCK_PORT="$MOCK_PORT" "$NODE" "$ROOT/harness/tests/mock_sudo_provider.mjs" > mock.log 2>&1 &
MPID=$!
( cd "$WORKDIR" && PATH="$WORKDIR/bin:$PATH" NYAXUS_FAKE_SUDO_PW="hunter2" \
    NYAXUS_FAKE_SUDO_MODE_FILE="$WORKDIR/fake-sudo-mode" \
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
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/profiles" \
    -H 'Content-Type: application/json' -d '{"id":"ops","name":"ops"}')"
[ "$CODE" = "200" ] || fail "ops profile create returned $CODE"
echo "healthy"

SID="$(curl -sf -X POST "$BASE/sessions" -H 'Content-Type: application/json' \
    -d '{"profile":"ops"}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"

# latest_input_id <sid> <rid> -> the input_id of the run's newest pending sudo gate
latest_input_id() {
    curl -sf "$BASE/sessions/$1" 2>/dev/null | "$NODE" -e "
        let d = '';
        process.stdin.on('data', (c) => { d += c; });
        process.stdin.on('end', () => {
            const s = JSON.parse(d);
            const rid = '$2';
            const reqs = (s.events || []).filter((e) =>
                e.event_type === 'input.requested' && e.payload.kind === 'sudo' && e.run_id === rid);
            process.stdout.write(reqs.length ? String(reqs[reqs.length - 1].payload.input_id || '') : '');
        });"
}

# approve_session <rid>: pass the first terminal approval for the whole session
approve_session() {
    local aid
    for _ in $(seq 1 75); do
        aid="$(curl -sf "$BASE/sessions/$SID" 2>/dev/null | "$NODE" -e '
            let d = "";
            process.stdin.on("data", (c) => { d += c; });
            process.stdin.on("end", () => {
                const s = JSON.parse(d);
                const reqs = (s.events || []).filter((e) => e.event_type === "approval.requested");
                process.stdout.write(reqs.length ? String(reqs[reqs.length - 1].payload.approval_id || "") : "");
            });')"
        [ -n "$aid" ] && break
        sleep 0.2
    done
    [ -n "$aid" ] || fail "the terminal call never opened its approval gate"
    local code
    code="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$1/commands" \
        -H 'Content-Type: application/json' \
        -d "{\"command\":\"approve\",\"approval_id\":\"$aid\",\"scope\":\"session\"}")"
    [ "$code" = "200" ] || fail "approval was not accepted (got $code)"
}

# wait_run_done <rid>: poll for the run's terminal event
# pw_exists <yes-or-no>: whether a nyaxus-sudo pwfile exists right now. The
# grant base is /dev/shm where it exists (Linux prod), else the temp dir the
# daemon's platform falls back to - the check probes both.
pw_exists() {
    if "$NODE" -e "
        const fs = require('fs');
        const os = require('os');
        const path = require('path');
        // In an MSYS shell os.tmpdir() is the sh-side temp; the win32 daemon's
        // temp_directory_path resolves the native %TEMP%. Probe both plus the
        // memory-backed preference.
        const dirs = ['/dev/shm', os.tmpdir()];
        if (process.env.LOCALAPPDATA) dirs.push(path.join(process.env.LOCALAPPDATA, 'Temp'));
        for (const dir of [...new Set(dirs)]) {
            try {
                if (fs.readdirSync(dir).some((f) => f.startsWith('nyaxus-sudo-') && f.endsWith('.pw'))) {
                    process.exit(0);
                }
            } catch {}
        }
        process.exit(1);
    "; then echo yes; else echo no; fi
}

wait_run_done() {
    for _ in $(seq 1 200); do
        curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q "\"run_id\":\"$1\"\(.*\)\?\"" && \
            curl -sf "$BASE/sessions/$SID" 2>/dev/null | "$NODE" -e '
                let d = "";
                process.stdin.on("data", (c) => { d += c; });
                process.stdin.on("end", () => {
                    const s = JSON.parse(d);
                    const rid = process.argv[1];
                    const done = (s.events || []).some((e) =>
                        e.run_id === rid &&
                        (e.event_type === "run.completed" || e.event_type === "run.cancelled" ||
                         e.event_type === "run.failed"));
                    process.exit(done ? 0 : 1);
                });' "$1" && return 0
        sleep 0.2
    done
    return 1
}

run_case() { # run_case <message>
    local rid
    rid="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$SID\",\"message\":\"$1\"}" \
        | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    echo "$rid"
}

echo "== run 1: gate -> answered once -> works, cleaned up =="
RID1="$(run_case "install the package")"
[ -n "$RID1" ] || fail "run 1 did not start"
approve_session "$RID1"
INPUT1=""
for _ in $(seq 1 75); do
    INPUT1="$(latest_input_id "$SID" "$RID1")"
    [ -n "$INPUT1" ] && break
    sleep 0.2
done
[ -n "$INPUT1" ] || fail "run 1's sudo gate never opened (see $WORKDIR/daemon.log)"
# The gate question carries the command for the operator's decision.
curl -sf "$BASE/sessions/$SID" | grep -q '"question":"Running this command as root needs the sudo password: sudo whoami"' \
    || fail "the sudo gate question must show the failed command"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RID1/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"sudo\",\"input_id\":\"$INPUT1\",\"password\":\"hunter2\",\"scope\":\"once\"}")"
[ "$CODE" = "200" ] || fail "run 1's password answer was rejected (got $CODE)"
wait_run_done "$RID1" || fail "run 1 did not finish"
# The retry went through askpass: the tool result carried sudo-ok.
"$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const reqs = JSON.parse(d);
        const toolText = (req) => (req.messages || []).filter((m) => m.role === 'tool').map((m) => String(m.content || '')).join('\n');
        const allToolText = reqs.map(toolText).join('\n');
        if (!allToolText.includes('sudo-ok whoami')) {
            console.error('run 1 tool result must carry the askpass retry output');
            process.exit(1);
        }
        // The password never rode a provider request either.
        for (const req of reqs) {
            if (JSON.stringify(req).includes('hunter2')) {
                console.error('the password leaked into a provider request');
                process.exit(1);
            }
        }
        console.log('run 1: askpass retry verified, password out of argv/transcript');
    });" < <(curl -sf "$MOCK/requests") || fail "run 1 retry"
# Once scope leaves nothing behind.
sleep 0.3
[ "$(pw_exists)" = "no" ] || fail "once scope must clean its grant files"
echo "run 1 verified"

echo "== run 2: gate -> wrong password -> rejected, nothing cached =="
RID2="$(run_case "check the id")"
[ -n "$RID2" ] || fail "run 2 did not start"
INPUT2=""
for _ in $(seq 1 75); do
    INPUT2="$(latest_input_id "$SID" "$RID2")"
    [ -n "$INPUT2" ] && break
    sleep 0.2
done
[ -n "$INPUT2" ] || fail "run 2's sudo gate never opened"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RID2/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"sudo\",\"input_id\":\"$INPUT2\",\"password\":\"definitely-wrong-pw\",\"scope\":\"once\"}")"
[ "$CODE" = "200" ] || fail "run 2's answer was rejected (got $CODE)"
wait_run_done "$RID2" || fail "run 2 did not finish"
"$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const reqs = JSON.parse(d);
        const toolText = (req) => (req.messages || []).filter((m) => m.role === 'tool').map((m) => String(m.content || '')).join('\n');
        const allToolText = reqs.map(toolText).join('\n');
        if (!allToolText.includes('incorrect password attempts')) {
            console.error('run 2 tool result must surface sudo rejection');
            process.exit(1);
        }
        if (!allToolText.includes('sudo rejected the password')) {
            console.error('run 2 must carry the rejection hint');
            process.exit(1);
        }
        console.log('run 2: wrong password rejected in-band');
    });" < <(curl -sf "$MOCK/requests") || fail "run 2 rejection"
sleep 0.3
[ "$(pw_exists)" = "no" ] || fail "a rejected answer must not leave a grant"
echo "run 2 verified"

echo "== run 3: gate -> answered ttl -> grant persists =="
RID3="$(run_case "whoami again")"
[ -n "$RID3" ] || fail "run 3 did not start"
INPUT3=""
for _ in $(seq 1 75); do
    INPUT3="$(latest_input_id "$SID" "$RID3")"
    [ -n "$INPUT3" ] && break
    sleep 0.2
done
[ -n "$INPUT3" ] || fail "run 3's sudo gate never opened"
CODE="$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE/runs/$RID3/commands" \
    -H 'Content-Type: application/json' \
    -d "{\"command\":\"sudo\",\"input_id\":\"$INPUT3\",\"password\":\"hunter2\",\"scope\":\"ttl\"}")"
[ "$CODE" = "200" ] || fail "run 3's answer was rejected (got $CODE)"
wait_run_done "$RID3" || fail "run 3 did not finish"
sleep 0.3
[ "$(pw_exists)" = "yes" ] || fail "the ttl grant must persist its pwfile"
echo "run 3 verified"

echo "== run 4: ttl grant covers it, no gate =="
RID4="$(run_case "list files")"
[ -n "$RID4" ] || fail "run 4 did not start"
wait_run_done "$RID4" || fail "run 4 did not finish"
IN4="$(curl -sf "$BASE/sessions/$SID" | "$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const s = JSON.parse(d);
        const rid = '$RID4';
        const gates = (s.events || []).filter((e) => e.event_type === 'input.requested' &&
            e.payload.kind === 'sudo' && 'run_id' in e && e.run_id === rid);
        process.stdout.write(String(gates.length));
    });")"
[ "$IN4" = "0" ] || fail "run 4 must ride the ttl grant with no gate (got $IN4 input.requested)"
"$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const reqs = JSON.parse(d);
        const toolText = (req) => (req.messages || []).filter((m) => m.role === 'tool').map((m) => String(m.content || '')).join('\n');
        const allToolText = reqs.map(toolText).join('\n');
        if (!allToolText.includes('sudo-ok ls')) {
            console.error('run 4 tool result must carry the direct grant success');
            process.exit(1);
        }
        console.log('run 4: ttl grant covered the call without a gate');
    });" < <(curl -sf "$MOCK/requests") || fail "run 4 direct"
echo "run 4 verified"

echo "== run 5: no-new-privileges hint, no gate =="
echo nnp > fake-sudo-mode
RID5="$(run_case "list processes")"
[ -n "$RID5" ] || fail "run 5 did not start"
wait_run_done "$RID5" || fail "run 5 did not finish"
IN5="$(curl -sf "$BASE/sessions/$SID" | "$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const s = JSON.parse(d);
        const rid = '$RID5';
        const gates = (s.events || []).filter((e) => e.event_type === 'input.requested' &&
            e.payload.kind === 'sudo' && 'run_id' in e && e.run_id === rid);
        process.stdout.write(String(gates.length));
    });")"
[ "$IN5" = "0" ] || fail "the nnp refusal must surface as a hint, not a gate (got $IN5)"
# The hint rides the tool result; the ledger keeps the whole story tickets-free
# of both password strings.
"$NODE" -e "
    let d = '';
    process.stdin.on('data', (c) => { d += c; });
    process.stdin.on('end', () => {
        const reqs = JSON.parse(d);
        const toolText = (req) => (req.messages || []).filter((m) => m.role === 'tool').map((m) => String(m.content || '')).join('\n');
        const allToolText = reqs.map(toolText).join('\n');
        if (!allToolText.includes('no new privileges') || !allToolText.includes('execution hint')) {
            console.error('run 5 must surface the no-new-privileges hint in-band');
            process.exit(1);
        }
        console.log('run 5: nnp hint verified');
    });" < <(curl -sf "$MOCK/requests") || fail "run 5 hint"

echo "== ledger: neither password ever appears =="
SESSION_JSON="$(curl -sf "$BASE/sessions/$SID")"
echo "$SESSION_JSON" | grep -q "hunter2" && fail "the correct password leaked into the ledger"
echo "$SESSION_JSON" | grep -q "definitely-wrong-pw" && fail "the wrong password leaked into the ledger"
echo "ledger clean"

echo
echo "SUDO_PASSWORD_TEST_PASSED"
