#!/usr/bin/env bash
# End-to-end check for the kv_cache_miss prompt-cache tripwire.
#
# Four daemons, each paired with a mock provider whose usage reports a long
# prompt (2048 tokens, above the 1024 tripwire floor):
#   - cold    -> known miss (cached_tokens=0)      -> exactly one kv_cache_miss, kind=cold
#   - hit     -> known hit  (cached_tokens=2000)   -> no kv_cache_miss, snapshot carries cached_tokens
#   - regress -> hit then miss in one run          -> exactly one kv_cache_miss, kind=regression
#   - unknown -> no cache fields in usage          -> no kv_cache_miss (unknown != miss)
#
# Usage: harness/tests/kv_cache_miss_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon.exe}"
NODE="${NODE:-/c/Program Files/nodejs/node.exe}"

COLD_MOCK_PORT=8991
COLD_DAEMON_PORT=8992
HIT_MOCK_PORT=8993
HIT_DAEMON_PORT=8994
REGRESS_MOCK_PORT=8995
REGRESS_DAEMON_PORT=8996
UNKNOWN_MOCK_PORT=8997
UNKNOWN_DAEMON_PORT=8998

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || { echo "node not found: $NODE" >&2; exit 1; }
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

# Each daemon gets its own CWD so the nyaxus.db files never contend.
WORKDIR_COLD="$(mktemp -d)"
WORKDIR_HIT="$(mktemp -d)"
WORKDIR_REGRESS="$(mktemp -d)"
WORKDIR_UNKNOWN="$(mktemp -d)"

start_pair() {
    local mode="$1" mock_port="$2" daemon_port="$3" workdir="$4"
    cd "$workdir" || exit 1
    MODE="$mode" MOCK_PORT="$mock_port" "$NODE" "$ROOT/harness/tests/mock_kv_cache_provider.mjs" > mock.log 2>&1 &
    echo $!
    e2e_port_free "$daemon_port"
    "$DAEMON" --port "$daemon_port" --provider openai \
        --llama "http://127.0.0.1:$mock_port/v1" --model mock-model > daemon.log 2>&1 &
    echo $!
}

COLD_IDS="$(start_pair cold "$COLD_MOCK_PORT" "$COLD_DAEMON_PORT" "$WORKDIR_COLD")"
HIT_IDS="$(start_pair hit "$HIT_MOCK_PORT" "$HIT_DAEMON_PORT" "$WORKDIR_HIT")"
REGRESS_IDS="$(start_pair regress "$REGRESS_MOCK_PORT" "$REGRESS_DAEMON_PORT" "$WORKDIR_REGRESS")"
UNKNOWN_IDS="$(start_pair unknown "$UNKNOWN_MOCK_PORT" "$UNKNOWN_DAEMON_PORT" "$WORKDIR_UNKNOWN")"

COLD_MPID="$(echo "$COLD_IDS" | sed -n 1p)"
COLD_DPID="$(echo "$COLD_IDS" | sed -n 2p)"
HIT_MPID="$(echo "$HIT_IDS" | sed -n 1p)"
HIT_DPID="$(echo "$HIT_IDS" | sed -n 2p)"
REGRESS_MPID="$(echo "$REGRESS_IDS" | sed -n 1p)"
REGRESS_DPID="$(echo "$REGRESS_IDS" | sed -n 2p)"
UNKNOWN_MPID="$(echo "$UNKNOWN_IDS" | sed -n 1p)"
UNKNOWN_DPID="$(echo "$UNKNOWN_IDS" | sed -n 2p)"

trap 'kill "$COLD_DPID" "$COLD_MPID" "$HIT_DPID" "$HIT_MPID" "$REGRESS_DPID" "$REGRESS_MPID" "$UNKNOWN_DPID" "$UNKNOWN_MPID" 2>/dev/null || true' EXIT
sleep 1.5

fail() { echo "FAIL: $1" >&2; exit 1; }

COLD_BASE="http://127.0.0.1:$COLD_DAEMON_PORT/api/v1"
HIT_BASE="http://127.0.0.1:$HIT_DAEMON_PORT/api/v1"
REGRESS_BASE="http://127.0.0.1:$REGRESS_DAEMON_PORT/api/v1"
UNKNOWN_BASE="http://127.0.0.1:$UNKNOWN_DAEMON_PORT/api/v1"

wait_health() {
    local base="$1"
    for _ in $(seq 1 200); do
        if curl -sf "$base/health" > /dev/null 2>&1; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

echo "== health =="
wait_health "$COLD_BASE" || fail "cold daemon did not come up (see $WORKDIR_COLD/daemon.log)"
wait_health "$HIT_BASE" || fail "hit daemon did not come up (see $WORKDIR_HIT/daemon.log)"
wait_health "$REGRESS_BASE" || fail "regress daemon did not come up (see $WORKDIR_REGRESS/daemon.log)"
wait_health "$UNKNOWN_BASE" || fail "unknown daemon did not come up (see $WORKDIR_UNKNOWN/daemon.log)"
echo "ok"

# start_and_wait <base> -> echoes "<terminal> <session_id>"
start_and_wait() {
    local base="$1"
    local sid run terminal
    sid="$(curl -sf -X POST "$base/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
    [ -n "$sid" ] || fail "no session id from $base"
    run="$(curl -sf -X POST "$base/sessions/$sid/runs" -H 'Content-Type: application/json' \
        -d "{\"session_id\":\"$sid\",\"message\":\"cache probe\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
    [ -n "$run" ] || fail "run did not start on $base"
    terminal=""
    for _ in $(seq 1 400); do
        if curl -sf "$base/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.failed"'; then
            terminal=failed
            break
        fi
        if curl -sf "$base/sessions/$sid" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
            terminal=completed
            break
        fi
        sleep 0.2
    done
    echo "$terminal $sid"
}

count_events() {
    curl -sf "$1/sessions/$2" | grep -o "\"event_type\":\"$3\"" | wc -l | tr -d ' '
}

check_run() {
    local label="$1" base="$2" want_miss="$3" want_kind="${4:-}"
    local terminal sid miss
    READLINE_OUT="$(start_and_wait "$base")"
    terminal="${READLINE_OUT%% *}"
    sid="${READLINE_OUT##* }"
    [ "$terminal" = "completed" ] || fail "$label: expected run.completed, got ${terminal:-none}"
    miss="$(count_events "$base" "$sid" kv_cache_miss)"
    if [ "$want_miss" = "0" ]; then
        [ "$miss" = "0" ] || fail "$label: expected no kv_cache_miss events, got $miss"
    else
        [ "$miss" = "1" ] || fail "$label: expected exactly one kv_cache_miss event, got $miss"
        curl -sf "$base/sessions/$sid" | grep -q "\"kind\":\"$want_kind\"" \
            || fail "$label: kv_cache_miss event does not name kind=$want_kind"
    fi
    # Echo the checked session so follow-up assertions look at the same run.
    echo "$sid"
}

echo "== cold miss (want one kv_cache_miss, kind=cold, daemon log alarm) =="
check_run cold "$COLD_BASE" 1 cold > /dev/null
grep -q "kv_cache_miss" "$WORKDIR_COLD/daemon.log" \
    || fail "cold: daemon log missing the kv_cache_miss warning"
echo "alarmed (kind=cold, logged)"

echo "== known hit (want no kv_cache_miss, snapshot carries cached_tokens) =="
HIT_SID="$(check_run hit "$HIT_BASE" 0)"
curl -sf "$HIT_BASE/sessions/$HIT_SID" | grep -q '"cached_tokens":"2000"' \
    || fail "hit: metrics.snapshot payload missing cached_tokens=2000"
echo "silent (cache hit, snapshot carries the count)"

echo "== hit-then-miss (want one kv_cache_miss, kind=regression, two provider calls) =="
REGRESS_SID="$(check_run regress "$REGRESS_BASE" 1 regression)"
SNAPSHOTS="$(count_events "$REGRESS_BASE" "$REGRESS_SID" metrics.snapshot)"
[ "$SNAPSHOTS" = "2" ] || fail "regress: expected 2 metrics.snapshot events (two provider calls), got $SNAPSHOTS"
curl -sf "$REGRESS_BASE/sessions/$REGRESS_SID" | grep -q '"event_type":"approval.auto"' \
    || fail "regress: the read-only ls call was not auto-approved (approval.auto missing)"
echo "alarmed (kind=regression, auto-approved ls in between)"

echo "== unknown cache data (want no kv_cache_miss) =="
check_run unknown "$UNKNOWN_BASE" 0 > /dev/null
echo "silent (no cache data is not a miss)"

echo
echo "KV_CACHE_MISS_TEST_PASSED"
