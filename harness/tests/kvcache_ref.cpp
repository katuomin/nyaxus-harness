// KV-cache correctness: splitting a sequence into prefix + suffix and running
// forward(prefix) then forward(suffix) with a KVState must produce the same
// final hidden state as forward(prefix+suffix) in one pass. The attended
// position set, the dot-product order, and the softmax are identical in both
// paths, so the results must be bit-identical (same FP ops, same order).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nyaxus/qwen3.hpp"

using namespace nyaxus::qwen3;

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "  PASS  " : "  FAIL  ", what.c_str());
    if (!cond) g_fail++;
}

int main() {
    const char* env = std::getenv("NYAXUS_EMBED_MODEL");
    std::string path = env && *env ? env
                                   : "models/nyx/qwen3-embedding-0.6b-int8.nyx";
    std::string err;
    Model m = load(path, err);
    if (!m.ok) {
        std::printf("could not load %s: %s\n", path.c_str(), err.c_str());
        return 2;
    }
    int H = m.hidden;

    const char* text =
        "the quick brown fox jumps over the lazy dog and then some more words "
        "to make the sequence long enough to split in the middle";
    std::vector<int> ids = encode(m, text);
    int S = int(ids.size());
    int split = S / 2;
    std::vector<int> prefix(ids.begin(), ids.begin() + split);
    std::vector<int> suffix(ids.begin() + split, ids.end());
    std::printf("seq=%d split=%d/%d\n", S, split, S - split);

    // Reference: one pass over the whole sequence.
    std::vector<float> hfull = forward(m, ids);
    const float* last_full = hfull.data() + std::size_t(S - 1) * H;

    // Cached: prefix pass fills the cache, suffix pass decodes over it.
    KVState state;
    state.k.resize(m.layers);
    state.v.resize(m.layers);
    forward(m, prefix, &state);
    std::vector<float> hpost = forward(m, suffix, &state);
    const float* last_cache = hpost.data() + std::size_t(suffix.size() - 1) * H;

    // Bit-identity of the final hidden state.
    bool exact = true;
    double maxdiff = 0.0;
    for (int j = 0; j < H; j++) {
        if (last_full[j] != last_cache[j]) exact = false;
        double d = std::abs(double(last_full[j]) - double(last_cache[j]));
        if (d > maxdiff) maxdiff = d;
    }
    check(exact, "cached final hidden state bit-identical to one-pass");
    std::printf("  max |diff| = %.3e\n", maxdiff);

    // The cache must be reusable: a third token decoded over the extended
    // cache must match a one-pass forward over prefix+suffix+[token].
    int extra = 14990;  // "hello"
    std::vector<int> ids3 = ids;
    ids3.push_back(extra);
    std::vector<float> hfull3 = forward(m, ids3);
    const float* last_full3 = hfull3.data() + std::size_t(ids3.size() - 1) * H;

    std::vector<int> one(1, extra);
    std::vector<float> hcache3 = forward(m, one, &state);
    // one-pass over [token] is a single row; `+ H - 1` would be the last
    // ELEMENT (and read past the buffer), not the last row.
    const float* last_cache3 = hcache3.data();

    bool exact3 = true;
    double maxdiff3 = 0.0;
    for (int j = 0; j < H; j++) {
        if (last_full3[j] != last_cache3[j]) exact3 = false;
        double d = std::abs(double(last_full3[j]) - double(last_cache3[j]));
        if (d > maxdiff3) maxdiff3 = d;
    }
    check(exact3, "third-token decode bit-identical to one-pass");
    std::printf("  max |diff| = %.3e\n", maxdiff3);

    std::printf(g_fail ? "FAILURES: %d\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
