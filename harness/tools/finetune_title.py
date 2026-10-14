#!/usr/bin/env python3
"""Fine-tune Qwen3-0.6B for chat-title generation, and evaluate it.

Pipeline (see docs/decisions/custom-qwen3-engine.md):
  decontam     training inputs vs TitleBench public inputs (disclosure)
  baseline     score the untrained base model on the eval subset
  train        LoRA SFT (response-only loss, plain continuation format)
  merge        merge LoRA, materialize lm_head (tied embeddings), verify
  eval-hf      batched HF generation on the eval set, write predictions
  eval-engine  format prompts for the C engine (title_gen), score output
  score        run TitleBench scorer.py on a predictions file

The prompt format below is the single source of truth - byte-identical
across training and every eval path.
"""
import argparse
import json
import math
import random
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

# ── Shared constants (train/eval must match byte-for-byte) ──────────────
PROMPT_FMT = "Generate a title: {message}\nTitle: "
MAX_NEW_TOKENS = 24
SEED = 42
MAX_INPUT_TOKENS = 1024      # eval input cap (truncations are reported)
TRAIN_MSG_TOKENS = 400       # training message cap (title + cue fit in 512)

ROOT = Path(__file__).resolve().parents[2]
TITLE_DIR = ROOT / "harness/models/safetensors/title"
BASE = TITLE_DIR / "Qwen3-0.6B"
TRAIN_RAW = TITLE_DIR / "finetune/chat-titles-filtered-115K/dataset.jsonl"
TRAIN_CLEAN = TITLE_DIR / "finetune/train-115k-clean.jsonl"
BENCH = TITLE_DIR / "finetune/TitleBench"
OUT_LORA = TITLE_DIR / "qwen3-title-lora"
OUT_MERGED = TITLE_DIR / "qwen3-title-merged"
RUNS = TITLE_DIR / "finetune/runs"

_PUNC = re.compile(r"[^a-z0-9\s]")


def normalize(text: str) -> str:
    return " ".join(_PUNC.sub(" ", text.lower()).split())


def tokens(text: str):
    return normalize(text).split()


def shingles(text: str, n: int = 5):
    t = tokens(text)
    return {" ".join(t[i:i + n]) for i in range(len(t) - n + 1)} if len(t) >= n else set()


def bench_rows():
    rows = []
    with open(BENCH / "test.jsonl", encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            if r.get("input"):
                rows.append(r)
    return rows


def eval_selection(n=None):
    """TitleBench protocol: shuffle row indices with random.Random(42), take N."""
    rows = bench_rows()
    idx = list(range(len(rows)))
    random.Random(SEED).shuffle(idx)
    sel = [rows[i] for i in (idx[:n] if n else idx)]
    return sel, len(rows)


def prompt_for(message: str) -> str:
    return PROMPT_FMT.format(message=message)


def write_predictions(path: Path, sel, titles):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        for r, t in zip(sel, titles):
            f.write(json.dumps({"output": t, "target": r["target"]}) + "\n")


def score(preds: Path):
    subprocess.run([sys.executable, str(BENCH / "scorer.py"), str(preds)], check=True)


# ── decontam ─────────────────────────────────────────────────────────────
def cmd_decontam(_a):
    pub = bench_rows()
    bench_norms = {normalize(r["input"]) for r in pub}
    # Rare 5-word shingles as the near-duplicate signal (a shingle shared by
    # many bench rows is boilerplate, not evidence of overlap).
    shingle_rows = Counter()
    for r in pub:
        for s in shingles(r["input"]):
            shingle_rows[s] += 1
    rare = {s for s, c in shingle_rows.items() if c <= 2}

    kept, exact, near = [], 0, 0
    with open(TRAIN_RAW, encoding="utf-8") as f:
        for line in f:
            r = json.loads(line)
            u = r.get("user") or ""
            if not u or not (r.get("title") or "").strip():
                continue
            if normalize(u) in bench_norms:
                exact += 1
                continue
            if len(shingles(u) & rare) >= 2:
                near += 1
                continue
            kept.append(r)
    TRAIN_CLEAN.parent.mkdir(parents=True, exist_ok=True)
    with open(TRAIN_CLEAN, "w", encoding="utf-8") as f:
        for r in kept:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    print(f"bench public rows: {len(pub)}")
    print(f"train rows: exact-dup removed={exact}  near-dup removed={near}  kept={len(kept)}")
    print(f"wrote {TRAIN_CLEAN}")
    print("method: normalized exact match + >=2 shared rare (<=2 bench rows) 5-word shingles")


# ── shared HF generation ─────────────────────────────────────────────────
def hf_generate(model, tok, prompts, batch=16):
    import torch
    titles = []
    tok.padding_side = "left"
    tok.truncation_side = "left"  # keep the trailing "Title: " cue
    dev = next(model.parameters()).device
    for i in range(0, len(prompts), batch):
        chunk = prompts[i:i + batch]
        enc = tok(chunk, return_tensors="pt", padding=True, truncation=True,
                  max_length=MAX_INPUT_TOKENS + 64)
        enc.input_ids = enc.input_ids.to(dev)
        enc.attention_mask = enc.attention_mask.to(dev)
        with torch.inference_mode():
            ids = model.generate(enc.input_ids, attention_mask=enc.attention_mask,
                                 max_new_tokens=MAX_NEW_TOKENS, do_sample=False,
                                 pad_token_id=tok.eos_token_id)
        new = ids[:, enc.input_ids.shape[1]:]
        titles.extend(tok.decode(t, skip_special_tokens=True).strip() for t in new)
    return titles


def load_hf(ckpt: Path, tok):
    import torch
    import transformers
    if not torch.cuda.is_available():
        print("WARNING: no CUDA visible - this will be very slow", file=sys.stderr)
    model = transformers.AutoModelForCausalLM.from_pretrained(
        str(ckpt), dtype=torch.bfloat16, low_cpu_mem_usage=True)
    model = model.to("cuda" if torch.cuda.is_available() else "cpu")
    model.eval()
    return model


# ── baseline / eval-hf ───────────────────────────────────────────────────
def cmd_baseline(a):
    import transformers
    sel, total = eval_selection(a.n)
    print(f"baseline: {len(sel)} of {total} public rows (seed {SEED})")
    tok = transformers.AutoTokenizer.from_pretrained(str(BASE))
    model = load_hf(BASE, tok)
    prompts = [prompt_for(r["input"]) for r in sel]
    titles = hf_generate(model, tok, prompts)
    p = RUNS / f"baseline-{len(sel)}.jsonl"
    write_predictions(p, sel, titles)
    print(f"wrote {p}")
    score(p)


def cmd_eval_hf(a):
    import transformers
    ckpt = Path(a.model) if a.model else OUT_MERGED
    if not ckpt.exists():
        sys.exit(f"checkpoint not found: {ckpt} (run merge first)")
    sel, total = eval_selection(a.n)
    print(f"eval-hf: {len(sel)} of {total} public rows (seed {SEED})")
    tok = transformers.AutoTokenizer.from_pretrained(str(ckpt))
    model = load_hf(ckpt, tok)
    prompts = [prompt_for(r["input"]) for r in sel]
    titles = hf_generate(model, tok, prompts)
    p = RUNS / f"hf-{len(sel)}.jsonl"
    write_predictions(p, sel, titles)
    print(f"wrote {p}")
    score(p)


# ── train ────────────────────────────────────────────────────────────────
def cmd_train(a):
    import torch
    import transformers
    from peft import LoraConfig, get_peft_model
    from trl import SFTConfig, SFTTrainer

    if not torch.cuda.is_available():
        sys.exit("no CUDA visible - training on CPU is not viable")
    train_path = Path(a.train) if a.train else TRAIN_CLEAN
    rows = []
    with open(train_path, encoding="utf-8") as f:
        for line in f:
            rows.append(json.loads(line))
    if a.n:
        random.Random(SEED).shuffle(rows)
        rows = rows[:a.n]
    print(f"train rows (pre-token-filter): {len(rows)} from {a.train}")

    tok = transformers.AutoTokenizer.from_pretrained(str(BASE))
    # Drop messages too long for the 512-token budget (message + cue + title).
    def fits(r):
        if len(tok(prompt_for(r["user"]), add_special_tokens=False)["input_ids"]) > TRAIN_MSG_TOKENS:
            return False
        return len(tok(r["title"], add_special_tokens=False)["input_ids"]) <= 32
    rows = [r for r in rows if fits(r)]
    print(f"train rows (after token filter): {len(rows)}")

    random.Random(SEED).shuffle(rows)
    n_val = max(1, len(rows) // 50)  # 2%
    val, train = rows[:n_val], rows[n_val:]
    from datasets import Dataset
    mk = lambda rs: Dataset.from_list(
        [{"prompt": prompt_for(r["user"]), "completion": r["title"]} for r in rs])
    train_ds, val_ds = mk(train), mk(val)

    model = transformers.AutoModelForCausalLM.from_pretrained(
        str(BASE), dtype=torch.bfloat16, low_cpu_mem_usage=True)
    lora = LoraConfig(r=32, lora_alpha=64, lora_dropout=0.0, bias="none", task_type="CAUSAL_LM",
                      target_modules=["q_proj", "k_proj", "v_proj", "o_proj",
                                      "gate_proj", "up_proj", "down_proj"])
    model = get_peft_model(model, lora)
    model.print_trainable_parameters()

    OUT_LORA.parent.mkdir(parents=True, exist_ok=True)
    # Scale warmup to the actual step count (the old fixed 11 was tuned for the
    # 12K set); 3% of the computed run keeps the cosine ramp sensible at any
    # dataset size.
    eff_batch = 8 * 4
    total_steps = (len(train) // eff_batch) * a.epochs
    warmup = max(11, int(total_steps * 0.03))
    cfg = SFTConfig(
        output_dir=str(OUT_LORA),
        per_device_train_batch_size=8,
        gradient_accumulation_steps=4,
        num_train_epochs=a.epochs,
        learning_rate=1e-4,
        lr_scheduler_type="cosine",
        warmup_steps=warmup,  # ~3% of the computed step count
        bf16=True,
        max_length=512,
        completion_only_loss=True,
        logging_steps=50,
        eval_strategy="steps",
        eval_steps=100,
        save_strategy="no",  # saved explicitly via trainer.save_model()
        seed=SEED,
        report_to=[],
        remove_unused_columns=False,
    )
    trainer = SFTTrainer(model=model, processing_class=tok, train_dataset=train_ds,
                         eval_dataset=val_ds, args=cfg)
    trainer.train()
    trainer.save_model(str(OUT_LORA))
    print(f"adapter saved to {OUT_LORA}")


# ── merge ────────────────────────────────────────────────────────────────
def cmd_merge(_a):
    import torch
    import torch.nn as nn
    import transformers
    from peft import PeftModel

    base = transformers.AutoModelForCausalLM.from_pretrained(
        str(BASE), dtype=torch.bfloat16, low_cpu_mem_usage=True)
    m = PeftModel.from_pretrained(base, str(OUT_LORA))
    m = m.merge_and_unload()
    # The base model ties lm_head to embed_tokens; the .nyx exporter requires
    # a distinct lm_head.weight tensor. transformers 5 drops state-dict entries
    # that share storage with another entry (pointer-based tie detection), so
    # the materialized weight must be a disjoint clone, not a shared reference.
    m.config.tie_word_embeddings = False
    m.lm_head = nn.Linear(m.config.hidden_size, m.config.vocab_size, bias=False)
    m.lm_head.weight = nn.Parameter(m.model.embed_tokens.weight.clone())
    OUT_MERGED.parent.mkdir(parents=True, exist_ok=True)
    m.save_pretrained(str(OUT_MERGED))
    transformers.AutoTokenizer.from_pretrained(str(BASE)).save_pretrained(str(OUT_MERGED))

    from safetensors import safe_open
    sf = OUT_MERGED / "model.safetensors"
    with safe_open(str(sf), framework="pt") as f:
        keys = set(f.keys())
    missing = {"lm_head.weight", "model.embed_tokens.weight", "model.norm.weight"} - keys
    if missing:
        sys.exit(f"merged checkpoint missing tensors: {sorted(missing)}")
    print(f"verified: lm_head.weight + embed + norm present in {sf}")
    print(f"merged checkpoint: {OUT_MERGED}")


# ── eval-engine ──────────────────────────────────────────────────────────
def cmd_eval_engine(a):
    sel, total = eval_selection(a.n)
    print(f"eval-engine: {len(sel)} of {total} public rows (seed {SEED})")
    RUNS.mkdir(parents=True, exist_ok=True)
    prompts = [prompt_for(r["input"]) for r in sel]
    pb = RUNS / "engine_prompts.bin"
    with open(pb, "wb") as f:
        f.write("\0".join(prompts).encode("utf-8") + b"\0")
    out = RUNS / "engine_titles.bin"
    print(f"wrote {pb}")
    print("build (if needed) + run from harness/:")
    print("  g++ -O2 -mavx -mfma -c src/qwen3_avx.cpp -o build-mingw/b_avx.o")
    print("  g++ -O2 -mavx2 -mfma -c src/qwen3_avx2.cpp -o build-mingw/b_avx2.o")
    print("  g++ -O2 -std=c++20 -mavx -mfma -static -Iinclude tools/title_gen.cpp src/qwen3.cpp "
          "src/qwen3_sse2.cpp build-mingw/b_avx.o build-mingw/b_avx2.o -o title_gen.exe")
    print(f"  title_gen.exe models\\nyx\\qwen3-title-0.6b.nyx {pb} {out}")
    print(f"then: python tools/finetune_title.py engine-preds --out {out} --n {a.n}")


def cmd_engine_preds(a):
    sel, _ = eval_selection(a.n)
    with open(a.out, "rb") as f:
        titles = f.read().decode("utf-8").split("\0")
    if titles and titles[-1] == "":
        titles.pop()  # trailing NUL
    if len(titles) != len(sel):
        print(f"WARNING: {len(titles)} titles for {len(sel)} prompts", file=sys.stderr)
    p = RUNS / f"engine-{len(sel)}.jsonl"
    write_predictions(p, sel, titles[:len(sel)])
    print(f"wrote {p}")
    score(p)


# ── score ────────────────────────────────────────────────────────────────
def cmd_score(a):
    score(Path(a.preds))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("decontam")
    p = sub.add_parser("baseline"); p.add_argument("--n", type=int, default=500)
    p = sub.add_parser("eval-hf"); p.add_argument("--n", type=int, default=None)
    p.add_argument("--model", default=None, help="checkpoint dir (default: merged)")
    p = sub.add_parser("train"); p.add_argument("--n", type=int, default=None)
    p.add_argument("--epochs", type=int, default=1)
    p.add_argument("--train", default=None,
                   help="training jsonl (default: the decontaminated 64K set)")
    sub.add_parser("merge")
    p = sub.add_parser("eval-engine"); p.add_argument("--n", type=int, default=30)
    p = sub.add_parser("engine-preds"); p.add_argument("--out", required=True)
    p.add_argument("--n", type=int, default=30)
    p = sub.add_parser("score"); p.add_argument("--preds", required=True)

    a = ap.parse_args()
    {"decontam": cmd_decontam, "baseline": cmd_baseline, "eval-hf": cmd_eval_hf,
     "train": cmd_train, "merge": cmd_merge, "eval-engine": cmd_eval_engine,
     "engine-preds": cmd_engine_preds, "score": cmd_score}[a.cmd](a)


if __name__ == "__main__":
    main()
