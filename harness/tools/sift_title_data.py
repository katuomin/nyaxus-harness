#!/usr/bin/env python3
"""Sift the decontaminated title training set.

Drops low-quality rows (multiple-choice, paragraph-completion, malformed)
from train-115k-clean.jsonl and writes train-115k-sifted.jsonl. The input is
already decontaminated against TitleBench; dropping rows keeps it decontaminated.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TITLE = ROOT / "harness/models/safetensors/title/finetune"
SRC = TITLE / "train-115k-clean.jsonl"
DST = TITLE / "train-115k-sifted.jsonl"

MCQ = re.compile(
    r"choose (your|the) (answer|correct|best|right)|"
    r"\bwhich of the following\b.*\b[a-d]\)\b|"
    r"^\s*question:.*\b[a-d]\)\b", re.I)
PARA = re.compile(
    r"what happens next in this paragraph|"
    r"how does the (next )?paragraph end|"
    r"complete the (following )?paragraph", re.I)
TOO_LONG_USER = 1600   # chars; task-def blobs are long and synthetic
MIN_USER = 8
MAX_TITLE = 60


def main():
    rows = [json.loads(l) for l in open(SRC, encoding="utf-8")]
    keep, drop = [], {}
    for r in rows:
        u = (r.get("user") or "").strip()
        t = (r.get("title") or "").strip()
        if len(u) < MIN_USER or not t:
            drop["malformed"] = drop.get("malformed", 0) + 1
            continue
        if MCQ.search(u):
            drop["mcq"] = drop.get("mcq", 0) + 1
            continue
        if PARA.search(u):
            drop["para"] = drop.get("para", 0) + 1
            continue
        if len(u) > TOO_LONG_USER:
            drop["too_long"] = drop.get("too_long", 0) + 1
            continue
        if len(t) > MAX_TITLE:
            drop["title_long"] = drop.get("title_long", 0) + 1
            continue
        keep.append(r)
    with open(DST, "w", encoding="utf-8") as f:
        for r in keep:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"input:  {len(rows)}")
    print(f"kept:   {len(keep)}")
    print(f"dropped: {drop}")
    print(f"wrote:  {DST}")


if __name__ == "__main__":
    main()
