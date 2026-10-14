#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nyaxus {
namespace qwen3 {

// A linear-projection weight in one of two storage modes (header `quant`):
//   quant=0: f32 holds the BF16 weights upcast to FP32.
//   quant=1: i8 holds per-row symmetric INT8 weights and scale holds one FP32
//            scale per output row; value = i8[o][k] * scale[o].
// Exactly one of the two data members is populated.
//
// Storage layout: both modes are [N, K] (the safetensors layout). The FP32
// kernels transpose a [BK, BN] tile into scratch per K chunk (amortized over
// all tokens); a [K, N] layout was tried and reverted - with N=1024 every
// k-row maps to the same L1 set ((k*4096/64) % 64 == 0) and the W stream
// thrashes L1, measured 2.4x slower. The AVX2 integer kernel also needs this
// layout directly: a vector holds 16 k-values of ONE neuron: W[n*K + k..k+15].
// scale: [N] per-neuron weight scale; value = i8[n][k] * scale[n].
struct Weight {
    std::vector<float> f32;
    std::vector<int8_t> i8;
    std::vector<float> scale;
};

// One transformer layer. Weights are stored row-major as [out, in] (the
// safetensors layout), upcast to FP32 at load time for correct arithmetic.
struct Layer {
    std::vector<float> input_ln;   // [hidden]
    Weight q_proj;                 // [heads*head_dim, hidden]
    Weight k_proj;                 // [kv*head_dim, hidden]
    Weight v_proj;                 // [kv*head_dim, hidden]
    Weight o_proj;                 // [hidden, heads*head_dim]
    std::vector<float> q_norm;     // [head_dim]
    std::vector<float> k_norm;     // [head_dim]
    std::vector<float> post_ln;    // [hidden]
    Weight gate_proj;              // [inter, hidden]
    Weight up_proj;                // [inter, hidden]
    Weight down_proj;              // [hidden, inter]
};

// A loaded Nyaxus Qwen3 model (embedding | reranker | generation).
struct Model {
    // Architecture.
    int hidden = 0;
    int layers = 0;
    int heads = 0;
    int kv_heads = 0;
    int head_dim = 0;
    int inter = 0;
    int max_pos = 0;
    int eos = 0;
    float rope_theta = 1000000.0f;
    float rms_eps = 1e-6f;
    int quant = 0;  // 0 = BF16 weights, 1 = INT8 projections + per-row scales

    // Task-specific.
    int task = 0;  // 0 embedding, 1 reranker, 2 generation
    int true_id = 0;
    int false_id = 0;

    int vocab_rows = 0;        // weight-matrix rows (may be padded)
    int tokenizer_size = 0;    // real token count

    // Weights.
    std::vector<float> embed;      // [vocab_rows, hidden]
    std::vector<float> final_norm; // [hidden]
    std::vector<Layer> layer;      // [layers]
    std::vector<float> lm_head;    // [vocab_rows, hidden] (generation)

    // Tokenizer.
    std::unordered_map<std::string, int> tok2id;
    std::vector<std::string> id2tok;  // id -> token (size tokenizer_size)
    std::vector<std::pair<int, int>> merges;  // (left_id, right_id) in rank order
    // (left_token, right_token) -> merge rank, built once in load().
    // Model-resident (not a process-wide static) so a second model with a
    // different tokenizer can't silently use the first one's ranks.
    std::map<std::pair<std::string, std::string>, int> merge_rank;

    bool ok = false;
    std::string error;
};

// Load a .nyx model file. Returns a Model with ok=true on success.
Model load(const std::string& path, std::string& error);

// Number of worker threads the engine uses for parallel GEMM (1 = the
// single-threaded path). Diagnostic only; override with NYAXUS_ENGINE_THREADS.
int engine_thread_count();

// --- Tokenizer ---
// Encode text into token ids using the model's BPE vocab + merges.
std::vector<int> encode(const Model& m, const std::string& text);
// Decode token ids back into text.
std::string decode(const Model& m, const std::vector<int>& ids);

// --- Inference ---
// Concurrency: thread-safe. The engine keeps shared scratch (the INT8 GEMM
// quantization buffer, the rerank_many KV cache) that is NOT thread-safe;
// every inference entry point (forward, embed_ids, rerank_ids, rerank,
// rerank_many, generate) serializes on a process-wide engine lock, so
// concurrent callers queue. encode/decode are const and lock-free.

// Incremental-decode state: per-layer cached K/V (K with RoPE applied), so
// generate() can prefill the prompt once and then decode one token at a
// time instead of re-running the whole sequence per step (~10-20x fewer
// FLOPs for a 500-token prompt + 20-token completion).
struct KVState {
    std::vector<std::vector<float>> k;  // [layers][cap][kv_heads*head_dim]
    std::vector<std::vector<float>> v;  // [layers][cap][kv_heads*head_dim]
    int pos = 0;  // positions cached
    int cap = 0;  // allocated positions
};

// Forward pass over token ids; returns the final (post-norm) hidden state at
// every position: [seq, hidden]. If state is non-null, the ids are at
// positions state->pos .. state->pos+seq-1, attention attends over the
// cached positions plus the current pass, the new K/V are appended to the
// cache, and state->pos advances by seq.
std::vector<float> forward(const Model& m, const std::vector<int>& ids,
                           KVState* state = nullptr);

// Embedding: last-token hidden state, L2-normalized. Returns hidden-dim vector.
std::vector<float> embed_ids(const Model& m, const std::vector<int>& ids);
std::vector<float> embed(const Model& m, const std::string& text);

// Reranker: score = logit(true) - logit(false) at the final position.
float rerank_ids(const Model& m, const std::vector<int>& query_ids,
                 const std::vector<int>& doc_ids);
float rerank(const Model& m, const std::string& query, const std::string& doc);
// Token count of the full rerank prompt (template + query + doc) - the
// actual forward length, which is larger than query+doc alone.
int rerank_prompt_tokens(const Model& m, const std::string& query,
                         const std::string& doc);

// Rerank several docs against one query, sharing the query-prefix KV cache:
// the template+query prefix is prefilled once, then each doc suffix is
// forwarded against the cached prefix (per-doc work drops from the whole
// prompt to just the doc suffix). Scores are identical to per-doc rerank()
// (bit-identical: same attended set, same op order).
std::vector<float> rerank_many(const Model& m, const std::string& query,
                               const std::vector<std::string>& docs);

// Generation (title model): greedy/temperature decode of up to max_new_tokens.
// no_leading_digit: block first tokens that decode to a leading digit (after
// any whitespace) at step 0. Policy, not engine semantics: the title model
// has a strong "lead with a number" prior, and when the source message
// carries no digits a leading-digit title cannot be grounded, so the daemon
// title path passes true in that case. Off by default.
std::string generate(const Model& m, const std::string& prompt,
                     int max_new_tokens, float temperature,
                     bool no_leading_digit = false);

}  // namespace qwen3
}  // namespace nyaxus
