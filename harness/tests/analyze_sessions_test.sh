#!/usr/bin/env bash
# End-to-end check for the analyze-sessions skill: build a small fixture
# nyaxus.db (two sessions, one a subagent; known token totals; one
# cp1252-byte message) and assert each script's output.
#
# Usage: harness/tests/analyze_sessions_test.sh
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SKILL="$ROOT/.agents/skills/analyze-sessions"
PYTHON="${PYTHON:-python}"

[ -f "$SKILL/SKILL.md" ] || { echo "skill not found: $SKILL" >&2; exit 1; }
command -v "$PYTHON" >/dev/null 2>&1 || { echo "python not found (needed for the skill scripts)" >&2; exit 1; }

WORKDIR="$(mktemp -d)"
cd "$WORKDIR" || exit 1
trap 'cd /; rmdir "$WORKDIR" 2>/dev/null' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

# Build the fixture ledger. Timestamps mirror the real ledger's mixed
# conventions: events.emitted_at is ISO-8601, sessions.updated_at /
# messages.created_at are epoch digits (one message uses nanoseconds to
# exercise the unit-normalization in show_session/search).
"$PYTHON" - <<'PY' || fail "fixture build failed"
import sqlite3, datetime as dt

def iso(y, m, d, H=0, M=0, S=0):
    return dt.datetime(y, m, d, H, M, S, tzinfo=dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")

def us(y, m, d, H=0, M=0, S=0):
    return str(int(dt.datetime(y, m, d, H, M, S, tzinfo=dt.timezone.utc).timestamp() * 1_000_000))

conn = sqlite3.connect("nyaxus.db")
conn.executescript("""
CREATE TABLE sessions (
  id TEXT PRIMARY KEY, profile TEXT NOT NULL DEFAULT 'orchestrator',
  model TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT 'new conversation',
  updated_at TEXT NOT NULL, context_mode TEXT NOT NULL DEFAULT 'selected',
  context_status TEXT NOT NULL DEFAULT 'ready', context_summary TEXT NOT NULL DEFAULT '',
  context_error TEXT NOT NULL DEFAULT '', source_session_id TEXT NOT NULL DEFAULT '',
  profile_revision INTEGER NOT NULL DEFAULT 0, profile_snapshot TEXT NOT NULL DEFAULT '',
  context_watermark INTEGER NOT NULL DEFAULT 0, pack_snapshot_id INTEGER NOT NULL DEFAULT 0,
  pack_revision INTEGER NOT NULL DEFAULT 0, md_log_path TEXT NOT NULL DEFAULT '');
CREATE TABLE messages (
  id INTEGER PRIMARY KEY AUTOINCREMENT, parent_id INTEGER NOT NULL DEFAULT 0,
  session_id TEXT NOT NULL, role TEXT NOT NULL, content TEXT NOT NULL,
  run_id TEXT NOT NULL DEFAULT '', created_at TEXT NOT NULL,
  reasoning TEXT NOT NULL DEFAULT '', is_active INTEGER NOT NULL DEFAULT 1);
CREATE TABLE events (
  id INTEGER PRIMARY KEY AUTOINCREMENT, event_id TEXT NOT NULL, session_id TEXT NOT NULL,
  run_id TEXT NOT NULL, parent_run_id TEXT NOT NULL DEFAULT '', seq INTEGER NOT NULL,
  event_type TEXT NOT NULL, emitted_at TEXT NOT NULL, payload TEXT NOT NULL DEFAULT '{}');
""")

# Session A: parent, model "alpha", 2 runs (1 completed 1000/200, 1 failed 0/0).
# Session B: subagent of A, model "beta", 1 run completed 500/50.
conn.execute("INSERT INTO sessions (id, model, title, updated_at, source_session_id) VALUES (?,?,?,?,?)",
             ("session-100", "alpha", "alpha work", us(2026, 9, 28), ""))
conn.execute("INSERT INTO sessions (id, model, title, updated_at, source_session_id) VALUES (?,?,?,?,?)",
             ("session-200", "beta", "subagent task", us(2026, 9, 28), "session-100"))

# Messages: A has a user prompt + assistant; B has a user (task description).
# One message carries a raw cp1252 byte (0xE9, invalid UTF-8 on its own)
# inserted as a hex literal so the TEXT cell holds the exact byte, to
# exercise the lenient text factory (a strict reader raises on it).
conn.execute("INSERT INTO messages (session_id, role, content, created_at) VALUES (?,?,?,?)",
             ("session-100", "user", "make the tests pass", us(2026, 9, 28)))
conn.execute("INSERT INTO messages (session_id, role, content, created_at) VALUES (?,?,?,?)",
             ("session-100", "assistant", "done", us(2026, 9, 28, 0, 1)))
conn.execute("INSERT INTO messages (session_id, role, content, created_at) VALUES (?,?,?,?)",
             ("session-200", "user", "subagent task description", us(2026, 9, 28, 0, 2)))
# 'caf\xE9 menu' as raw bytes: 63 61 66 E9 20 6D 65 6E 75. Also stored with a
# nanosecond timestamp (19 digits) to exercise unit normalization.
ns_ts = str(int(dt.datetime(2026, 9, 28, 0, 3, tzinfo=dt.timezone.utc).timestamp() * 1_000_000_000))
# CAST(BLOB AS TEXT) re-tags the bytes as TEXT (no re-encoding), so the cell
# holds the exact invalid-UTF-8 bytes as a TEXT value -- the same shape as
# the real ledger's cp1252 console output.
conn.execute("INSERT INTO messages (session_id, role, content, created_at) VALUES (?,?,CAST(X'636166E9206D656E75' AS TEXT),?)",
             ("session-100", "assistant", ns_ts))

# Events: run totals for the cost rollup.
conn.execute("INSERT INTO events (event_id, session_id, run_id, seq, event_type, emitted_at, payload) VALUES (?,?,?,?,?,?,?)",
             ("e1", "session-100", "run-1", 1, "run.completed", iso(2026, 9, 28), '{"total_prompt_tokens":"1000","total_completion_tokens":"200","provider_calls":"1"}'))
conn.execute("INSERT INTO events (event_id, session_id, run_id, seq, event_type, emitted_at, payload) VALUES (?,?,?,?,?,?,?)",
             ("e2", "session-100", "run-2", 1, "run.failed", iso(2026, 9, 28), '{"error":"provider stream failed","total_prompt_tokens":"0","total_completion_tokens":"0","provider_calls":"0"}'))
conn.execute("INSERT INTO events (event_id, session_id, run_id, seq, event_type, emitted_at, payload) VALUES (?,?,?,?,?,?,?)",
             ("e3", "session-200", "run-3", 1, "run.completed", iso(2026, 9, 28), '{"total_prompt_tokens":"500","total_completion_tokens":"50","provider_calls":"1"}'))
conn.execute("INSERT INTO events (event_id, session_id, run_id, seq, event_type, emitted_at, payload) VALUES (?,?,?,?,?,?,?)",
             ("e4", "session-100", "run-1", 2, "metrics.snapshot", iso(2026, 9, 28), '{"prompt_tokens":"1000","completion_tokens":"200","total_tokens":"1200"}'))
conn.commit()
conn.close()
print("fixture built")
PY
[ -f nyaxus.db ] || fail "fixture db missing"

# --- cost.py: subagent included by default (alpha 1200 + beta 550 = 1750) ---
OUT="$("$PYTHON" "$SKILL/cost.py" --db nyaxus.db 2>&1)"
echo "$OUT" | grep -q "TOTAL" || fail "cost.py: no TOTAL line"
echo "$OUT" | grep -q "total     1.8k" || fail "cost.py: grand total not 1.8k (subagent should be included): $OUT"
echo "$OUT" | grep -q "provider stream failed" || true

# --- cost.py --exclude-subagents: only alpha (1200) ---
OUTX="$("$PYTHON" "$SKILL/cost.py" --db nyaxus.db --exclude-subagents 2>&1)"
echo "$OUTX" | grep -q "total     1.2k" || fail "cost.py --exclude-subagents: expected 1.2k: $OUTX"

# --- prompts.py: subagent excluded by default (only 'make the tests pass') ---
OUTP="$("$PYTHON" "$SKILL/prompts.py" --db nyaxus.db 2>&1)"
echo "$OUTP" | grep -q "make the tests pass" || fail "prompts.py: missing user prompt: $OUTP"
echo "$OUTP" | grep -q "subagent task description" && fail "prompts.py: subagent prompt should be excluded by default"

# --- prompts.py --include-subagents: now includes the task description ---
OUTPI="$("$PYTHON" "$SKILL/prompts.py" --db nyaxus.db --include-subagents 2>&1)"
echo "$OUTPI" | grep -q "subagent task description" || fail "prompts.py --include-subagents: missing task description: $OUTPI"

# --- search.py: finds the prompt; --events finds the failure payload ---
OUTS="$("$PYTHON" "$SKILL/search.py" --db nyaxus.db --grep "make the tests pass" 2>&1)"
echo "$OUTS" | grep -q "make the tests pass" || fail "search.py: no message match: $OUTS"
OUTSE="$("$PYTHON" "$SKILL/search.py" --db nyaxus.db --grep "provider stream failed" --events 2>&1)"
echo "$OUTSE" | grep -q "provider stream failed" || fail "search.py --events: no event match: $OUTSE"

# --- search.py: the cp1252 message is found and renders without crashing ---
OUTC="$("$PYTHON" "$SKILL/search.py" --db nyaxus.db --grep "menu" 2>&1)"
echo "$OUTC" | grep -q "menu" || fail "search.py: cp1252 message not found / crashed: $OUTC"

# --- show_session.py: renders session A, includes the cp1252 assistant msg ---
OUTSH="$("$PYTHON" "$SKILL/show_session.py" --db nyaxus.db --session 100 2>&1)"
echo "$OUTSH" | grep -q "alpha work" || fail "show_session.py: missing title: $OUTSH"
echo "$OUTSH" | grep -q "menu" || fail "show_session.py: cp1252 message not rendered: $OUTSH"

echo
echo "ANALYZE_SESSIONS_TEST_PASSED"
