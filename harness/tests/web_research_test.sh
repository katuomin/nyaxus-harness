#!/usr/bin/env bash
# web_research_test.sh - web.search / web.fetch / web.use without the
# per-call approval gate, plus the grounded-citations source ledger and the
# Firecrawl routing (https scrape + search fallback).
#
# Covers:
#   - a run whose model calls web.search -> web.fetch -> web.use completes
#     without any approve/deny command (all three are read-only)
#   - web.fetch returns the page as readable text: scripts/styles stripped,
#     entities decoded, and the [src:N] id present
#   - the fetch lands in the session's source ledger (GET /api/v1/sources)
#     with used=true after web.use
#   - web.search falls back to Firecrawl /v1/search when SearXNG returns
#     nothing
#   - web.fetch routes an https:// URL through Firecrawl /v1/scrape and
#     records the result in the ledger
#
# Usage: harness/tests/web_research_test.sh [path-to-nyaxus_daemon]
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
source "$ROOT/harness/tests/lib_e2e.sh"
DAEMON="${1:-$ROOT/harness/nyaxus_daemon_linux}"
NODE="${NODE:-node}"
WEB_PORT=9021
PROVIDER_PORT=9022
DAEMON_PORT=9023
BASE="http://127.0.0.1:$DAEMON_PORT/api/v1"

[ -f "$DAEMON" ] || { echo "daemon binary not found: $DAEMON" >&2; exit 1; }
[ -f "$NODE" ] || command -v "$NODE" >/dev/null 2>&1 || { echo "node not found: $NODE" >&2; exit 1; }
# The daemon runs from a temp CWD below, so a relative path must be resolved now.
case "$DAEMON" in
    /*|*:/) ;;
    *) DAEMON="$(pwd)/$DAEMON" ;;
esac

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1

fail() { echo "FAIL: $1" >&2; exit 1; }

MOCK_PORT="$WEB_PORT" "$NODE" "$ROOT/harness/tests/mock_web_server.mjs" > web.log 2>&1 &
WEBPID=$!
MOCK_PORT="$PROVIDER_PORT" WEB_PORT="$WEB_PORT" \
    "$NODE" "$ROOT/harness/tests/mock_web_research_provider.mjs" > provider.log 2>&1 &
PROVPID=$!
e2e_port_free "$DAEMON_PORT"
NYAXUS_WEB_SEARCH_ENDPOINT="http://127.0.0.1:$WEB_PORT" \
NYAXUS_WEB_FIRECRAWL_ENDPOINT="http://127.0.0.1:$WEB_PORT" \
"$DAEMON" --port "$DAEMON_PORT" --provider openai \
    --llama "http://127.0.0.1:$PROVIDER_PORT/v1" --model mock-model > daemon.log 2>&1 &
DPID=$!
trap 'kill "$DPID" "$WEBPID" "$PROVPID" 2>/dev/null || true' EXIT
sleep 1.5

echo "== health =="
curl -sf "$BASE/health" > /dev/null || fail "daemon did not come up (see $WORKDIR/daemon.log)"
echo "ok"

echo "== run calls web.search -> web.fetch -> web.use; no approval needed =="
SID="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID" ] || fail "no session id"
RUN1="$(curl -sf -X POST "$BASE/sessions/$SID/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID\",\"message\":\"what does the web research test page say?\"}" | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN1" ] || fail "run did not start"

DONE=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE=1; break
    fi
    if curl -sf "$BASE/sessions/$SID" 2>/dev/null | grep -q '"event_type":"run.waiting_approval"'; then
        fail "a web tool still sits behind the approval gate (run is waiting for a decision)"
    fi
    sleep 0.2
done
[ "$DONE" = "1" ] || fail "run did not complete without approval (see $WORKDIR/daemon.log)"
echo "run completed ungated"

SESSION_JSON="$(curl -sf "$BASE/sessions/$SID")"

echo "== web.fetch returned readable page text with a [src:N] id =="
printf '%s' "$SESSION_JSON" | grep -q "Hello from the mock web" \
    || fail "web.fetch result is missing the page text"
printf '%s' "$SESSION_JSON" | grep -q '\[src:1\]' \
    || fail "web.fetch result is missing the [src:N] id"
echo "page text + source id present"

echo "== script/style content is stripped, entities are decoded =="
if printf '%s' "$SESSION_JSON" | grep -q "should-not-appear"; then
    fail "script content leaked into the extracted text"
fi
printf '%s' "$SESSION_JSON" | grep -q '<quoted>' \
    || fail "double-escaped entity was not decoded"
echo "extraction clean"

echo "== web.use marked the source as used =="
printf '%s' "$SESSION_JSON" | grep -q "Recorded used: 1 source" \
    || fail "web.use did not record the used source"
echo "web.use recorded"

echo "== the final answer cites the source =="
printf '%s' "$SESSION_JSON" | grep -q "hello from the web research test. \[src:1\]" \
    || fail "the final answer does not carry the [src:1] citation"
echo "citation present"

echo "== the source ledger holds the fetch, marked used =="
SOURCES="$(curl -sf "$BASE/sources?session_id=$SID")"
printf '%s' "$SOURCES" | grep -q "\"url\":\"http://127.0.0.1:$WEB_PORT/page.html\"" \
    || fail "source ledger is missing the fetched url: $SOURCES"
printf '%s' "$SOURCES" | grep -q '"used":true' \
    || fail "source ledger does not show used=true: $SOURCES"
printf '%s' "$SOURCES" | grep -q '"status":"ok"' \
    || fail "source ledger does not show status ok: $SOURCES"
echo "ledger correct"

echo "== second run: web.search falls back to Firecrawl, web.fetch routes https =="
SID2="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^\"]*\)".*/\1/p')"
[ -n "$SID2" ] || fail "no second session id"
RUN2="$(curl -sf -X POST "$BASE/sessions/$SID2/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID2\",\"message\":\"find the firecrawl page\"}" | sed -n 's/.*"run_id":"\([^\"]*\)".*/\1/p')"
[ -n "$RUN2" ] || fail "second run did not start"

DONE2=""
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID2" 2>/dev/null | grep -q '"event_type":"run.completed"'; then
        DONE2=1; break
    fi
    if curl -sf "$BASE/sessions/$SID2" 2>/dev/null | grep -q '"event_type":"run.waiting_approval"'; then
        fail "a web tool still sits behind the approval gate (run is waiting for a decision)"
    fi
    sleep 0.2
done
[ "$DONE2" = "1" ] || fail "second run did not complete without approval (see $WORKDIR/daemon.log)"
echo "second run completed ungated"

SESSION2_JSON="$(curl -sf "$BASE/sessions/$SID2")"

echo "== web.search fell back to Firecrawl (SearXNG returned nothing) =="
printf '%s' "$SESSION2_JSON" | grep -q "Firecrawl Result" \
    || fail "the firecrawl fallback search result is missing"
echo "fallback search result present"

echo "== web.fetch routed the https URL through Firecrawl =="
printf '%s' "$SESSION2_JSON" | grep -q "Firecrawl scraped page" \
    || fail "the firecrawl-scraped markdown is missing"
printf '%s' "$SESSION2_JSON" | grep -q '\[src:2\]' \
    || fail "the https fetch is missing the [src:2] id"
echo "https fetch via firecrawl present"

echo "== web.use marked the firecrawl source as used =="
printf '%s' "$SESSION2_JSON" | grep -q "Recorded used: 1 source" \
    || fail "web.use did not record the firecrawl source"
echo "web.use recorded"

echo "== the source ledger holds the https fetch (firecrawl), marked used =="
SOURCES2="$(curl -sf "$BASE/sources?session_id=$SID2")"
printf '%s' "$SOURCES2" | grep -q '"url":"https://example.com/firecrawl-page"' \
    || fail "source ledger is missing the https url: $SOURCES2"
printf '%s' "$SOURCES2" | grep -q '"used":true' \
    || fail "source ledger does not show used=true: $SOURCES2"
printf '%s' "$SOURCES2" | grep -q '"status":"ok"' \
    || fail "source ledger does not show status ok: $SOURCES2"
echo "ledger correct"

# ── the read-budget pager (web.fetch page=N) ───────────────────────────
# A page whose extraction overruns the per-page budget: page 1 (the default
# read) names the remaining pages in-band; the page=2 re-fetch reads the
# rest and reports no continuation.
SID3="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID3" ] || fail "no pager session id"
RUN3="$(curl -sf -X POST "$BASE/sessions/$SID3/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID3\",\"message\":\"read the big page in full\"}" \
    | sed -n 's/.*"run_id":"\([^\"]*\)".*/\1/p')"
[ -n "$RUN3" ] || fail "the pager run did not start"
for _ in $(seq 1 200); do
    if curl -sf "$BASE/sessions/$SID3" 2>/dev/null | grep -q '"run.completed"'; then break; fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID3" | grep -q '"run.completed"' \
    || fail "the pager run did not complete"

fetch_results() {
    curl -sf "$BASE/sessions/$SID3" | PE="$1" "$NODE" -e '
        let d = "";
        process.stdin.on("data", (c) => { d += c; });
        process.stdin.on("end", () => {
            const events = JSON.parse(d).events.filter(e => e.event_type === "tool.completed" && e.payload && e.payload.name === "web.fetch");
            const ev = events[Number(process.env.PE) - 1];
            process.stdout.write(ev ? ev.payload.result : "");
        });
    '
}
P1="$(fetch_results 1)"
P2="$(fetch_results 2)"
[ -n "$P1" ] || fail "the pager fetches did not run"
printf '%s' "$P1" | grep -q "SECTION-1 " \
    || fail "page 1 does not carry the first section: $P1"
printf '%s' "$P1" | grep -q "SECTION-13" \
    && fail "page 1 leaked the second page's content: $P1"
printf '%s' "$P1" | grep -q "extraction continues: 1 more page(s) available" \
    || fail "page 1 lacks the continuation note: $P1"
printf '%s' "$P1" | grep -q "page=2" \
    || fail "page 1's note does not name the next page: $P1"
printf '%s' "$P2" | grep -q "SECTION-13" \
    || fail "page 2 does not carry the second page's sections: $P2"
printf '%s' "$P2" | grep -q "(page 2)" \
    || fail "page 2 does not label itself: $P2"
printf '%s' "$P2" | grep -q "extraction continues" \
    && fail "page 2 announced a continuation that does not exist: $P2"
echo "pager: page 1 named the next page, page 2 read the rest"

# ── big-search regression ────────────────────────────────────────────
# A SearXNG-shaped JSON body that overruns a single 8 KiB read (header +
# body in one segment, as a LAN instance sends it). The daemon's client
# used to append the first chunk twice and truncate the tail of the
# document, so the final result is the marker: it must survive.
SID4="$(curl -sf -X POST "$BASE/sessions" -d '{}' | sed -n 's/.*"id":"\([^"]*\)".*/\1/p')"
[ -n "$SID4" ] || fail "no big-search session id"
RUN4="$(curl -sf -X POST "$BASE/sessions/$SID4/runs" -H 'Content-Type: application/json' \
    -d "{\"session_id\":\"$SID4\",\"message\":\"search the big list\"}" \
    | sed -n 's/.*"run_id":"\([^"]*\)".*/\1/p')"
[ -n "$RUN4" ] || fail "the big-search run did not start"
for _ in $(seq 1 75); do
    if curl -sf "$BASE/sessions/$SID4" 2>/dev/null | grep -q '"run.completed"'; then break; fi
    sleep 0.2
done
curl -sf "$BASE/sessions/$SID4" | grep -q '"run.completed"' \
    || fail "the big-search run did not complete"
BIG_SEARCH_RESULT="$(curl -sf "$BASE/sessions/$SID4" | "$NODE" -e '
    let d = "";
    process.stdin.on("data", (c) => { d += c; });
    process.stdin.on("end", () => {
        const events = JSON.parse(d).events.filter(e => e.event_type === "tool.completed" && e.payload && e.payload.name === "web.search");
        process.stdout.write(events.length ? events[events.length - 1].payload.result : "");
    });
')"
[ -n "$BIG_SEARCH_RESULT" ] || fail "the big search did not run"
printf '%s' "$BIG_SEARCH_RESULT" | grep -q "Big search result 8" \
    || fail "the big search document did not parse end to end (double-append regression): $BIG_SEARCH_RESULT"
printf '%s' "$BIG_SEARCH_RESULT" | grep -q "Big search result 1" \
    || fail "the head of the big search response is missing"
printf '%s' "$BIG_SEARCH_RESULT" | grep -q "Firecrawl Result" \
    && fail "the big search fell back to Firecrawl (the SearXNG leg failed to parse)"
echo "big search: full document parsed (no fallback)"

echo
echo "WEB_RESEARCH_TEST_PASSED"
