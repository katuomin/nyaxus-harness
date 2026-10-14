// First-step digit block in generate(): a synthetic model whose greedy
// first token is a digit-leading one must pick the next-best token when
// no_leading_digit is set, and the default (flag off) path must be
// untouched. Model-independent: the Model is hand-built, no .nyx load.
//
// Design: every embedding row is [1,0,0,0] and the single layer's
// projections are all zero, so the residual stream is the constant
// [1,0,0,0] and RMSNorm keeps it there. Logits are then the lm_head's
// first column, which we rank by hand:
//   "2023" (id 3) = 10 > " 5" (id 4) = 8 > "1st" (id 5) = 6 > "cat" (id 1) = 5
// Greedy therefore starts on the digit token "2023"; the mask must block
// ALL digit-leading tokens (bare, space-prefixed, ordinal) and drop to
// "cat".
#include <iostream>
#include <string>

#include "nyaxus/qwen3.hpp"

using namespace nyaxus::qwen3;

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
    std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
    if (!cond) g_fail++;
}

int main() {
    Model m;
    m.hidden = 4;
    m.layers = 1;
    m.heads = 1;
    m.kv_heads = 1;
    m.head_dim = 4;
    m.inter = 4;
    m.max_pos = 64;
    m.eos = 7;
    m.rope_theta = 1000000.0f;
    m.rms_eps = 1e-6f;
    m.quant = 0;
    m.task = 2;
    m.vocab_rows = 8;
    m.tokenizer_size = 8;
    m.ok = true;

    // 0 <unk> (encode fallback), 1 cat, 2 dog, 3 "2023", 4 " 5", 5 "1st",
    // 6 zzz, 7 <eos>.
    m.id2tok = {"<unk>", "cat", "dog", "2023", " 5", "1st", "zzz", "<eos>"};
    for (int i = 0; i < 8; i++) m.tok2id[m.id2tok[i]] = i;
    m.merges.clear();
    m.merge_rank.clear();

    m.embed.assign(8 * 4, 0.0f);
    for (int i = 0; i < 8; i++) m.embed[i * 4] = 1.0f;
    m.final_norm = {1.0f, 1.0f, 1.0f, 1.0f};
    m.layer.resize(1);
    Layer& L = m.layer[0];
    L.input_ln.assign(4, 1.0f);
    L.q_norm.assign(4, 1.0f);
    L.k_norm.assign(4, 1.0f);
    L.post_ln.assign(4, 1.0f);
    L.q_proj.f32.assign(4 * 4, 0.0f);
    L.k_proj.f32.assign(4 * 4, 0.0f);
    L.v_proj.f32.assign(4 * 4, 0.0f);
    L.o_proj.f32.assign(4 * 4, 0.0f);
    L.gate_proj.f32.assign(4 * 4, 0.0f);
    L.up_proj.f32.assign(4 * 4, 0.0f);
    L.down_proj.f32.assign(4 * 4, 0.0f);

    m.lm_head.assign(8 * 4, 0.0f);
    m.lm_head[3 * 4] = 10.0f;  // "2023"
    m.lm_head[4 * 4] = 8.0f;   // " 5"
    m.lm_head[5 * 4] = 6.0f;   // "1st"
    m.lm_head[1 * 4] = 5.0f;   // "cat"

    const std::string prompt = "Generate a title: hi there\nTitle: ";

    std::string plain = generate(m, prompt, 2, 0.0f);
    check(plain.rfind("2023", 0) == 0,
          "no mask: greedy first token is the digit token (got '" + plain + "')");

    std::string masked = generate(m, prompt, 2, 0.0f, true);
    check(masked.rfind("cat", 0) == 0,
          "mask: all digit-leading first tokens blocked, next-best wins "
          "(got '" + masked + "')");

    // The mask only affects step 0: the second token of the masked decode
    // may be a digit token again (here it is: "2023" wins step 1 freely).
    check(masked.find("2023") != std::string::npos,
          "mask: later steps are unmasked (got '" + masked + "')");

    std::string deflt = generate(m, prompt, 2, 0.0f);
    check(deflt == plain, "default flag leaves the plain path untouched");

    if (g_fail) {
        std::cout << g_fail << " FAILURES\n";
        return 1;
    }
    std::cout << "DIGIT_MASK_REF_PASSED\n";
    return 0;
}
