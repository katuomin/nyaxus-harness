#!/usr/bin/env python3
"""Build-time exporter: Hugging Face safetensors -> Nyaxus Qwen3 model file.

Pure standard library (no torch/transformers/safetensors). Reads the source
checkpoint once, normalizes tensor names, and emits a single memory-mappable
binary the C++ runtime loads directly. The daemon never parses safetensors.

Usage:
    python export_qwen3.py <model_dir> <task> <out.nyx> [quant]

    task in {embedding, reranker, generation}
    quant in {0, 1} (default 0)
        0 = BF16 weights (reference quality)
        1 = per-row symmetric INT8 for the seven linear projections
            (q/k/v/o/gate/up/down); embed_tokens, norms, and lm_head stay
            BF16. Each quantized tensor gains a `<name>.scale` FP32 tensor
            with one scale per output row (value = int8 * scale).

The emitted format is documented in docs/decisions/custom-qwen3-engine.md and
by the header layout in nyaxus/qwen3.hpp.
"""

import array
import json
import struct
import sys
from pathlib import Path

MAGIC = 0x3358594E  # "NYX3"
VERSION = 1
ARCH_QWEN3 = 1

TASK_EMBEDDING = 0
TASK_RERANKER = 1
TASK_GENERATION = 2

TASKS = {
    "embedding": TASK_EMBEDDING,
    "reranker": TASK_RERANKER,
    "generation": TASK_GENERATION,
}

QUANT_BF16 = 0
QUANT_INT8 = 1

# The seven linear projections eligible for INT8 (everything else stays BF16).
PROJ_SUFFIXES = (
    ".q_proj.weight",
    ".k_proj.weight",
    ".v_proj.weight",
    ".o_proj.weight",
    ".gate_proj.weight",
    ".up_proj.weight",
    ".down_proj.weight",
)


def read_safetensors(path):
    """Return (metadata, {name: (dtype, shape, data_bytes)})."""
    with open(path, "rb") as f:
        raw = f.read()
    header_len = struct.unpack_from("<Q", raw, 0)[0]
    header = json.loads(raw[8:8 + header_len])
    data_start = 8 + header_len
    metadata = header.get("__metadata__", {})
    tensors = {}
    for name, info in header.items():
        if name == "__metadata__":
            continue
        start = info["data_offsets"][0]
        end = info["data_offsets"][1]
        tensors[name] = (info["dtype"], info["shape"], raw[data_start + start:data_start + end])
    return metadata, tensors


def normalize_name(name):
    """Map repo-specific prefixes to the single internal naming scheme."""
    if name.startswith("model."):
        name = name[len("model."):]
    return name


def expected_tensors(task, layers):
    """The exact internal tensor set the runtime expects, in load order."""
    names = ["embed_tokens.weight", "norm.weight"]
    for i in range(layers):
        names += [
            "layers.%d.input_layernorm.weight" % i,
            "layers.%d.self_attn.q_proj.weight" % i,
            "layers.%d.self_attn.k_proj.weight" % i,
            "layers.%d.self_attn.v_proj.weight" % i,
            "layers.%d.self_attn.o_proj.weight" % i,
            "layers.%d.self_attn.q_norm.weight" % i,
            "layers.%d.self_attn.k_norm.weight" % i,
            "layers.%d.post_attention_layernorm.weight" % i,
            "layers.%d.mlp.gate_proj.weight" % i,
            "layers.%d.mlp.up_proj.weight" % i,
            "layers.%d.mlp.down_proj.weight" % i,
        ]
    if task == TASK_GENERATION:
        names.append("lm_head.weight")
    return names


def bf16_bytes_to_f32(data):
    """Decode little-endian BF16 bytes into an array('f')."""
    u16 = array.array("H")
    u16.frombytes(data)
    u32 = array.array("I", (b << 16 for b in u16))
    f32 = array.array("f")
    f32.frombytes(u32.tobytes())
    return f32


def quantize_tensor(shape, data):
    """Per-row symmetric INT8 of a BF16 [rows, cols] tensor.

    scale[r] = max|row r| / 127, so |int8| <= 127 and the row's magnitude is
    preserved to ~8 bits. Returns (int8_bytes, f32_scale_bytes).
    """
    rows, cols = shape
    out = bytearray(rows * cols)
    scales = bytearray(rows * 4)
    for r in range(rows):
        f32 = bf16_bytes_to_f32(data[2 * r * cols:2 * (r + 1) * cols])
        maxabs = 0.0
        for v in f32:
            a = -v if v < 0 else v
            if a > maxabs:
                maxabs = a
        if maxabs == 0.0:
            continue  # all-zero row: scale stays 0.0, quantized row stays 0
        scale = maxabs / 127.0
        struct.pack_into("<f", scales, r * 4, scale)
        inv = 1.0 / scale
        q = array.array("b")
        for v in f32:
            t = v * inv
            qi = int(t + (0.5 if t >= 0 else -0.5))
            if qi > 127:
                qi = 127
            elif qi < -127:
                qi = -127
            q.append(qi)
        out[r * cols:(r + 1) * cols] = q.tobytes()
    return bytes(out), bytes(scales)


def export(model_dir, task, out_path, quant=QUANT_BF16):
    model_dir = Path(model_dir)
    config = json.loads((model_dir / "config.json").read_text(encoding="utf-8"))
    tok = json.loads((model_dir / "tokenizer.json").read_text(encoding="utf-8"))

    hidden = config["hidden_size"]
    layers = config["num_hidden_layers"]
    heads = config["num_attention_heads"]
    kv_heads = config["num_key_value_heads"]
    head_dim = config.get("head_dim", hidden // heads)
    inter = config["intermediate_size"]
    max_pos = config["max_position_embeddings"]
    eos = config.get("eos_token_id", 151645)
    rope_theta = float(config.get("rope_theta", 1000000.0))
    rms_eps = float(config.get("rms_norm_eps", 1e-6))

    # tokenizer.json's model.vocab is token->id and excludes the special
    # added_tokens; merge them for the full, contiguous id space.
    vocab = dict(tok["model"]["vocab"])
    for at in tok.get("added_tokens", []):
        vocab[at["content"]] = at["id"]
    id_to_token = {v: k for k, v in vocab.items()}
    merges = tok["model"]["merges"]
    tokenizer_size = len(vocab)

    # Reranker true/false token ids come from the 1_LogitScore config.
    true_id = 0
    false_id = 0
    if task == TASK_RERANKER:
        lc = json.loads((model_dir / "1_LogitScore" / "config.json").read_text(encoding="utf-8"))
        true_id = lc["true_token_id"]
        false_id = lc["false_token_id"]

    # Resolve merge components to ids up front (all must be in the vocab).
    merge_pairs = []
    for left, right in merges:
        if left not in vocab or right not in vocab:
            raise SystemExit("merge component not in vocab: %r %r" % (left, right))
        merge_pairs.append((vocab[left], vocab[right]))

    metadata, tensors = read_safetensors(model_dir / "model.safetensors")
    normalized = {}
    for name, (dtype, shape, data) in tensors.items():
        nname = normalize_name(name)
        if nname in normalized:
            raise SystemExit("duplicate tensor after normalization: %s" % nname)
        normalized[nname] = (dtype, shape, data)

    wanted = expected_tensors(task, layers)
    missing = [n for n in wanted if n not in normalized]
    if missing:
        raise SystemExit("missing tensors: %s" % missing)
    # Every stored tensor must be one we expect (no surprises).
    extra = [n for n in normalized if n not in wanted]
    if extra:
        raise SystemExit("unexpected tensors: %s" % extra)

    # Sanity-check shapes against the config so a bad checkpoint fails loudly.
    def check(name, expect):
        got = list(normalized[name][1])
        if got != expect:
            raise SystemExit("shape mismatch %s: got %s want %s" % (name, got, expect))

    check("embed_tokens.weight", [tokenizer_size_rows(config, task), hidden])
    check("norm.weight", [hidden])
    check("layers.0.self_attn.q_proj.weight", [heads * head_dim, hidden])
    check("layers.0.self_attn.k_proj.weight", [kv_heads * head_dim, hidden])
    check("layers.0.self_attn.v_proj.weight", [kv_heads * head_dim, hidden])
    check("layers.0.self_attn.o_proj.weight", [hidden, heads * head_dim])
    check("layers.0.self_attn.q_norm.weight", [head_dim])
    check("layers.0.self_attn.k_norm.weight", [head_dim])
    check("layers.0.mlp.gate_proj.weight", [inter, hidden])
    check("layers.0.mlp.up_proj.weight", [inter, hidden])
    check("layers.0.mlp.down_proj.weight", [hidden, inter])
    if task == TASK_GENERATION:
        check("lm_head.weight", [tokenizer_size_rows(config, task), hidden])

    vocab_rows = tokenizer_size_rows(config, task)

    # ---- Serialize ----
    # Header layout (little-endian), 20 u32 then 2 f32 = 88 bytes:
    #   magic version arch task
    #   hidden_size layer_count attention_heads kv_heads head_dim intermediate_size
    #   tokenizer_size vocab_rows max_position_embeddings eos_token_id
    #   quant true_token_id false_token_id
    #   num_tensors vocab_entries num_merges
    #   rope_theta(f32) rms_norm_eps(f32)
    #
    # Followed by, in order (counts from the header, no offsets needed):
    #   vocab_entries * [u32 id, u32 len, utf8 bytes]
    #   num_merges    * [u32 left_id, u32 right_id]
    #   num_tensors   * [u32 name_len, name, u32 ndim, ndim*u32 dims,
    #                    u64 data_offset, u64 data_len]   (tensor index)
    #   concatenated tensor data (bf16; int8 for quantized projections;
    #   f32 for `<name>.scale` tensors)
    #
    # In INT8 mode the seven linear projections become raw INT8 and each gains
    # a `<name>.scale` FP32 tensor (one scale per output row).
    blobs = []
    for name in wanted:
        dtype, shape, data = normalized[name]
        if quant == QUANT_INT8 and name.endswith(PROJ_SUFFIXES):
            if name.endswith(".q_proj.weight"):
                # One progress line per layer: pure-Python quantization of the
                # 0.6B checkpoint takes a couple of minutes.
                print("  quantizing layer %s/27..." % name.split(".")[1],
                      flush=True)
            qdata, scales = quantize_tensor(shape, data)
            blobs.append((name, shape, qdata))
            blobs.append((name + ".scale", [shape[0]], scales))
        else:
            blobs.append((name, shape, data))
    num_tensors = len(blobs)
    out = bytearray()
    out += struct.pack(
        "<20Iff",
        MAGIC, VERSION, ARCH_QWEN3, task,
        hidden, layers, heads, kv_heads, head_dim, inter,
        tokenizer_size, vocab_rows, max_pos, eos,
        quant,
        true_id, false_id,
        num_tensors, len(vocab), len(merge_pairs),
        rope_theta, rms_eps,
    )

    # vocab: (id, utf8) pairs, sorted by id for a stable layout.
    for tid in sorted(id_to_token.keys()):
        s = id_to_token[tid].encode("utf-8")
        out += struct.pack("<II", tid, len(s))
        out += s

    for left_id, right_id in merge_pairs:
        out += struct.pack("<II", left_id, right_id)

    index_entries = []
    for name, shape, data in blobs:
        name_b = name.encode("utf-8")
        index_entries.append((name_b, shape, data))

    index_len = 0
    for name_b, shape, _ in index_entries:
        index_len += 4 + len(name_b) + 4 + 4 * len(shape) + 8 + 8
    data_base = len(out) + index_len

    cursor = data_base
    for name_b, shape, data in index_entries:
        out += struct.pack("<I", len(name_b))
        out += name_b
        out += struct.pack("<I", len(shape))
        for d in shape:
            out += struct.pack("<I", d)
        out += struct.pack("<Q", cursor)
        out += struct.pack("<Q", len(data))
        cursor += len(data)

    for _, _, data in index_entries:
        out += data

    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(bytes(out))

    total_mb = len(out) / (1024 * 1024)
    print("wrote %s (%.1f MB)" % (out_path, total_mb))
    print("  task=%s layers=%d hidden=%d heads=%d/%d head_dim=%d inter=%d"
          % (task, layers, hidden, heads, kv_heads, head_dim, inter))
    print("  quant=%s tokenizer_size=%d vocab_rows=%d merges=%d tensors=%d"
          % ("int8" if quant == QUANT_INT8 else "bf16", tokenizer_size,
             vocab_rows, len(merge_pairs), num_tensors))
    if task == TASK_RERANKER:
        print("  true_token_id=%d false_token_id=%d" % (true_id, false_id))
    return out_path


def tokenizer_size_rows(config, task):
    """Weight-matrix row count. The title checkpoint pads to a larger value."""
    return config["vocab_size"]


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        sys.exit(2)
    model_dir, task, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
    if task not in TASKS:
        print("unknown task: %s" % task)
        sys.exit(2)
    quant = QUANT_BF16
    if len(sys.argv) == 5:
        q = sys.argv[4]
        if q in ("0", "bf16"):
            quant = QUANT_BF16
        elif q in ("1", "int8"):
            quant = QUANT_INT8
        else:
            print("unknown quant: %s (want 0/bf16 or 1/int8)" % q)
            sys.exit(2)
    export(model_dir, TASKS[task], out_path, quant)
