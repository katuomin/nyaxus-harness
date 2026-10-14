// Isolate the S=1 cached-decode divergence (kvcache_ref "third token" check).
//
// T1: the single-threaded S=1 GEMM path (nyaxus_gemm_avx) vs a scalar
//     reference, on the real layer-0 q_proj weights.
// T2: the same input row through the threaded S=26 path - the two must agree
//     (same arithmetic, different dispatch).
// T3: control - the full-engine S=1 cached decode vs one-pass (known FAIL).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nyaxus/qwen3.hpp"

using namespace nyaxus::qwen3;

// The kernels are extern "C" (linkable); gemm/rmsnorm are in an anonymous
// namespace in qwen3.cpp, so we drive the kernel directly and inline the
// (trivial) rmsnorm.
extern "C" void nyaxus_gemm_avx(const float* X, int S, int K, const float* W,
                                int N, float* Y, float* scratch);
extern "C" void nyaxus_gemm_range_avx(const float* X, int S, int K,
                                      const float* W, int N, float* Y,
                                      float* scratch, int b0, int b1);

static void rmsnorm(const float* x, int n, const std::vector<float>& w,
                    float eps, float* y) {
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float inv = 1.0f / std::sqrt(ss / n + eps);
    for (int i = 0; i < n; i++) y[i] = x[i] * inv * w[i];
}

static double compare(const float* a, const float* b, int n, bool* exact) {
    double maxd = 0.0;
    *exact = true;
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) *exact = false;
        double d = std::abs(double(a[i]) - double(b[i]));
        if (d > maxd) maxd = d;
    }
    return maxd;
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
    int QD = m.heads * m.head_dim;

    const char* text =
        "the quick brown fox jumps over the lazy dog and then some more words "
        "to make the sequence long enough to split in the middle";
    std::vector<int> ids = encode(m, text);
    int S = int(ids.size());
    int split = S / 2;
    std::vector<int> prefix(ids.begin(), ids.begin() + split);
    std::vector<int> suffix(ids.begin() + split, ids.end());
    const int extra = 14990;

    KVState state;
    state.k.resize(m.layers);
    state.v.resize(m.layers);
    forward(m, prefix, &state);
    forward(m, suffix, &state);
    std::printf("seq=%d split=%d/%d cached=%d\n", S, split, S - split,
                state.pos);

    // Input row for the extra token, after layer-0 pre-norm.
    std::vector<float> h(H);
    const float* e = m.embed.data() + size_t(extra) * H;
    for (int j = 0; j < H; j++) h[j] = e[j];
    const Layer& L0 = m.layer[0];
    std::vector<float> hnorm(H);
    rmsnorm(h.data(), H, L0.input_ln, m.rms_eps, hnorm.data());

    const Weight& W = L0.q_proj;
    // Wt tile (64*8) + Cbuf (S*8) for the largest S used below (26).
    std::vector<float> scratch(64 * 8 + 26 * 8);

    // --- T1: S=1 single-threaded GEMM vs scalar reference ---
    std::vector<float> ref(QD), out1(QD);
    for (int n = 0; n < QD; n++) {
        float acc = 0.0f;
        const float* wrow = W.f32.data() + size_t(n) * H;
        for (int j = 0; j < H; j++) acc += hnorm[j] * wrow[j];
        ref[n] = acc;
    }
    nyaxus_gemm_avx(hnorm.data(), 1, H, W.f32.data(), QD, out1.data(),
                    scratch.data());
    bool exact = false;
    double maxd = compare(out1.data(), ref.data(), QD, &exact);
    std::printf("T1 S=1 GEMM vs scalar ref: %s max|diff|=%.3e\n",
                exact ? "PASS" : "FAIL", maxd);

    // --- T2: same row through the threaded S=26 path ---
    std::vector<float> X26(size_t(26) * H);
    for (int s = 0; s < 26; s++)
        for (int j = 0; j < H; j++) X26[size_t(s) * H + j] = hnorm[j];
    std::vector<float> out26(size_t(26) * QD);
    // Full-range call: per-block arithmetic is identical to the threaded
    // path (each neuron block is independent), so this exercises the same
    // 2-token-blocked kernel the pool uses.
    nyaxus_gemm_range_avx(X26.data(), 26, H, W.f32.data(), QD, out26.data(),
                          scratch.data(), 0, (QD + 7) / 8);
    maxd = compare(out26.data() + 25 * QD, out1.data(), QD, &exact);
    std::printf("T2 S=1 vs S=26 threaded, same row: %s max|diff|=%.3e\n",
                exact ? "PASS" : "FAIL", maxd);

    // --- T3: full engine, layer-by-layer - find the first diverging layer ---
    std::vector<int> ids3 = ids;
    ids3.push_back(extra);
    _putenv_s("NYAXUS_DEBUG_LAYERS", "dbg_full.bin");
    std::vector<float> hfull3 = forward(m, ids3);
    _putenv_s("NYAXUS_DEBUG_LAYERS", "dbg_cached.bin");
    std::vector<int> one(1, extra);
    std::vector<float> hcache3 = forward(m, one, &state);
    _putenv_s("NYAXUS_DEBUG_LAYERS", "");
    const float* last_full3 = hfull3.data() + size_t(ids3.size() - 1) * H;
    const float* last_cache3 = hcache3.data();  // single row
    maxd = compare(last_cache3, last_full3, H, &exact);
    std::printf("T3 engine S=1 cached vs one-pass: %s max|diff|=%.3e\n",
                exact ? "PASS" : "FAIL", maxd);

    auto read_dump = [](const char* p) {
        std::vector<float> v;
        FILE* f = std::fopen(p, "rb");
        if (!f) return v;
        std::fseek(f, 0, SEEK_END);
        long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        v.resize(static_cast<std::size_t>(n) / 4);
        std::fread(v.data(), sizeof(float), v.size(), f);
        std::fclose(f);
        return v;
    };
    std::vector<float> df = read_dump("dbg_full.bin");
    std::vector<float> dc = read_dump("dbg_cached.bin");
    std::printf("dumps: full=%zu (expect %zu) cached=%zu (expect %zu)\n",
                df.size(), static_cast<std::size_t>(m.layers) * 26 * H, dc.size(),
                static_cast<std::size_t>(m.layers) * H);
    int first_bad = -1;
    for (int l = 0; l < m.layers; l++) {
        const float* rf = df.data() + (size_t(l) * 26 + 25) * H;
        const float* rc = dc.data() + size_t(l) * H;
        double ld = 0.0;
        bool lexact = true;
        for (int j = 0; j < H; j++) {
            if (rf[j] != rc[j]) lexact = false;
            double d = std::abs(double(rf[j]) - double(rc[j]));
            if (d > ld) ld = d;
        }
        if (!lexact) {
            std::printf("layer %2d: max|diff|=%.4e\n", l, ld);
            if (first_bad < 0) first_bad = l;
        }
    }
    std::printf("first diverging layer: %d\n", first_bad);
    return 0;
}
