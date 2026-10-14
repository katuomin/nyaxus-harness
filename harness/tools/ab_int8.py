#!/usr/bin/env python3
"""A/B accuracy check: BF16 vs INT8 .nyx embeddings for the same sentences.

Usage:
    python ab_int8.py <smoke_exe> <bf16_model.nyx> <int8_model.nyx>

Runs the smoke test's `dumptext` mode on a fixed set of sentences with each
model and reports cosine similarity and max absolute difference per sentence.
"""

import math
import os
import subprocess
import sys

SENTENCES = [
    "The capital of France is Paris",
    "How do I reset a WiFi router",
    "Nyaxus stores memories in SQLite with FTS5 full-text search",
    "The quick brown fox jumps over the lazy dog",
    "Qwen3 uses grouped query attention with rotary embeddings",
    "Deploy the daemon with a 2048 token recall budget",
]


def embed(smoke_exe, model, text):
    env = dict(os.environ, NYAXUS_EMBED_MODEL=model)
    out = subprocess.run(
        [smoke_exe, "dumptext", text],
        capture_output=True, text=True, env=env, check=True).stdout
    return [float(x) for x in out.split()]


def main():
    smoke_exe, bf16, i8 = sys.argv[1], sys.argv[2], sys.argv[3]
    worst_cos = 1.0
    worst_mad = 0.0
    for s in SENTENCES:
        a = embed(smoke_exe, bf16, s)
        b = embed(smoke_exe, i8, s)
        dot = sum(x * y for x, y in zip(a, b))
        na = math.sqrt(sum(x * x for x in a))
        nb = math.sqrt(sum(x * x for x in b))
        cos = dot / (na * nb)
        mad = max(abs(x - y) for x, y in zip(a, b))
        worst_cos = min(worst_cos, cos)
        worst_mad = max(worst_mad, mad)
        print("cos=%.6f  max_abs_diff=%.6f  %r" % (cos, mad, s))
    print("---")
    print("worst cosine=%.6f (drift %.4f%%)  worst max_abs_diff=%.6f"
          % (worst_cos, (1 - worst_cos) * 100, worst_mad))


if __name__ == "__main__":
    main()
