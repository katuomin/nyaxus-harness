#!/usr/bin/env python3
"""One-off 27B title seed batch (distribution-matched training data).

The downloaded 115K set is mostly synthetic task prompts; the 27B's titles
over *natural* messages are the missing distribution. This script collects
candidate messages (real session openers from the ledger + a conversational
sample of the sifted 115K), decontaminates them against TitleBench, and
titles them with the 27B using the exact training prompt format.

Output: finetune/seed/chat-title-seed.jsonl with keys `user` + `title`,
ready to merge into the training set. The daemon's live chat.title harvest
lands in a separate file (title-harvest/chat-title-harvest.jsonl).

Usage:
  python harness/tools/seed_title_batch.py [--n 150] [--endpoint URL]
      [--model NAME] [--out PATH] [--dry-run]
"""
import argparse
import json
import random
import re
import sqlite3
import sys
import urllib.request
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from finetune_title import (PROMPT_FMT, ROOT, SEED, bench_rows, normalize,
                            shingles)  # noqa: E402

TITLE = ROOT / "harness/models/safetensors/title/finetune"
SIFTED = TITLE / "train-115k-sifted.jsonl"
OUT = TITLE / "seed/chat-title-seed.jsonl"
DB = ROOT / "nyaxus.db"

# Task-prompt markers: the sifted set keeps short synthetic prompts (math,
# NLI, QA, summarize). Real conversation does not read like these.
TASKY = re.compile(
    r"^\s*(question|q):|summarize|answer (the|this|the following)|"
    r"choose |select from|complete the|translate|write (a|an|one)|"
    r"what is the answer|which of the|passage|article about|dialogue:|"
    r"briefly|one sentence|premise|hypothesis|entail|multiple[- ]choice|"
    r"true or false|fill in the blank", re.I)
MIN_LEN = 20
MAX_LEN = 500


def real_openers() -> list[str]:
    """First user message of every session in the ledger (+ backup)."""
    openers = []
    for db in (DB, DB.with_name("nyaxus.db.bak-20260906")):
        if not db.exists():
            continue
        try:
            con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
            rows = con.execute(
                "SELECT session_id, content FROM messages m WHERE role='user' "
                "AND id = (SELECT MIN(id) FROM messages WHERE session_id = m.session_id)"
            ).fetchall()
            con.close()
            openers.extend(c.strip() for _s, c in rows if c and c.strip())
        except sqlite3.Error:
            pass
    return openers


def conversational_sample(n: int) -> list[str]:
    rows = []
    with open(SIFTED, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            u = (r.get("user") or "").strip()
            if not (MIN_LEN <= len(u) <= MAX_LEN):
                continue
            if TASKY.search(u):
                continue
            rows.append(u)
    random.Random(SEED).shuffle(rows)
    return rows[:n]


def decontaminate(messages: list[str]) -> list[str]:
    pub = bench_rows()
    bench_norms = {normalize(r["input"]) for r in pub}
    shingle_rows = Counter()
    for r in pub:
        for s in shingles(r["input"]):
            shingle_rows[s] += 1
    rare = {s for s, c in shingle_rows.items() if c <= 2}
    kept = []
    seen = set()
    for u in messages:
        nu = normalize(u)
        if not nu or nu in seen:
            continue
        if nu in bench_norms or len(shingles(u) & rare) >= 2:
            continue
        seen.add(nu)
        kept.append(u)
    return kept


def strip_leading_year(message: str, title: str) -> str:
    if len(title) < 4 or not title[:4].isdigit() or (len(title) > 4 and title[4].isdigit()):
        return title
    year = int(title[:4])
    if not (1900 <= year <= 2099) or title[:4] in message:
        return title
    cut = 4
    if cut < len(title) and title[cut] == "-":
        j, digits = cut + 1, 0
        while j < len(title) and title[j].isdigit() and digits < 2:
            j += 1
            digits += 1
        if digits == 2:
            cut = j
            if cut + 3 <= len(title) and title[cut] == "-" and title[cut + 1].isdigit() and title[cut + 2].isdigit():
                cut += 3
    if cut < len(title) and title[cut] in " -":
        cut += 1
    return title[cut:]


def strip_leading_count(message: str, title: str) -> str:
    if not title or not title[0].isdigit():
        return title
    i = 0
    while i < len(title) and title[i].isdigit():
        i += 1
    if not (1 <= i <= 4):
        return title
    j = i
    if j + 2 <= len(title) and title[j:j + 2] in ("st", "nd", "rd", "th") and (j + 2 == len(title) or title[j + 2] == " "):
        j += 2
    if j < len(title) and title[j] != " ":
        return title
    token = title[:j]
    if token in message:
        return title
    rest = title[j + 1:] if j < len(title) else ""
    return rest or title


def title_with_27b(endpoint: str, model: str, message: str, timeout: float = 120.0) -> str:
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": PROMPT_FMT.format(message=message)}],
        "max_tokens": 24,
        "temperature": 0.0,
        "stream": False,
        # The 27B is a thinking model: without this, reasoning eats the
        # 24-token budget and content comes back empty (same knob the
        # daemon's no_thinking provider config sends).
        "chat_template_kwargs": {"enable_thinking": False},
    }).encode("utf-8")
    req = urllib.request.Request(
        endpoint.rstrip("/") + "/chat/completions",
        data=body,
        headers={"content-type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        data = json.loads(resp.read().decode("utf-8"))
    text = data["choices"][0]["message"]["content"]
    text = text.splitlines()[0] if text else ""
    text = text.strip()
    text = strip_leading_year(message, text)
    text = strip_leading_count(message, text)
    return text[:200]


def main():
    # Windows console defaults to cp1252 when redirected; titles and
    # messages are UTF-8, so a single non-Latin character would crash the
    # print. Keep the batch alive on any character.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--n", type=int, default=150, help="target number of seed rows")
    ap.add_argument("--endpoint", default="http://<deployment-lan>:8080/v1")
    ap.add_argument("--model", default="/models/Swift-Qwen3.8-27B-Uncensored-BF16.i1-Q4_K_M.gguf")
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--timeout", type=float, default=600.0,
                    help="per-request timeout (the 27B is shared with the "
                         "daemon's own inference, so queued requests are slow)")
    ap.add_argument("--dry-run", action="store_true", help="collect candidates, no model calls")
    a = ap.parse_args()

    messages = real_openers()
    need = max(0, a.n - len(messages))
    messages += conversational_sample(need + 20)  # slack for decontam/dupes
    messages = decontaminate(messages)[:a.n]
    print(f"candidates: {len(messages)} (real openers + conversational sample)")
    if not messages:
        sys.exit("no candidates survived decontamination")
    if a.dry_run:
        for m in messages[:10]:
            print("  sample:", m[:90].replace("\n", " "))
        print(f"  ... {len(messages)} total, would title with {a.model}")
        return

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    ok, empty = 0, 0
    with open(out, "w", encoding="utf-8") as f:
        for i, m in enumerate(messages):
            try:
                t = title_with_27b(a.endpoint, a.model, m, timeout=a.timeout)
            except Exception as e:  # keep going: one bad row does not kill the batch
                print(f"  [{i + 1}/{len(messages)}] error: {e}", file=sys.stderr)
                continue
            if not t:
                empty += 1
                continue
            f.write(json.dumps({"user": m, "title": t}, ensure_ascii=False) + "\n")
            ok += 1
            print(f"  [{i + 1}/{len(messages)}] {t}")
    print(f"wrote {ok} rows ({empty} empty) to {out}")


if __name__ == "__main__":
    main()
