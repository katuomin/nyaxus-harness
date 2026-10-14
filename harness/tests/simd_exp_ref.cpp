// Reference test for the SIMD attention-softmax and SwiGLU kernels
// (src/qwen3_avx.cpp / src/qwen3_sse2.cpp). The fast exp uses a degree-5
// Taylor polynomial for 2^f on [-0.5, 0.5] (~3e-6 max error), so the SIMD
// outputs must match a scalar std::exp reference within 1e-4 (generous
// tolerance; the actual error is ~1e-6 after normalization).
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

extern "C" void nyaxus_softmax_avx(const float* x, int n, float maxlogit,
                                   float* out);
extern "C" void nyaxus_softmax_sse2(const float* x, int n, float maxlogit,
                                    float* out);
extern "C" void nyaxus_swiglu_avx(const float* gate, const float* up, int n,
                                  float* out);
extern "C" void nyaxus_swiglu_sse2(const float* gate, const float* up, int n,
                                   float* out);
extern "C" int nyaxus_cpu_has_avx_fma();

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "  PASS  " : "  FAIL  ", what.c_str());
    if (!cond) g_fail++;
}

static void softmax_ref(const float* x, int n, float maxlogit, float* out) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        out[i] = std::exp(x[i] - maxlogit);
        sum += out[i];
    }
    for (int i = 0; i < n; i++) out[i] /= sum;
}

static void swiglu_ref(const float* gate, const float* up, int n, float* out) {
    for (int i = 0; i < n; i++) {
        float g = gate[i];
        float sig = 1.0f / (1.0f + std::exp(-g));
        out[i] = g * sig * up[i];
    }
}

static float max_abs(const std::vector<float>& a, const std::vector<float>& b) {
    float m = 0.0f;
    for (std::size_t i = 0; i < a.size(); i++) {
        float d = std::fabs(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

static void run_case(int n, unsigned seed, bool use_avx) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, 4.0f);
    std::vector<float> x(n), gate(n), up(n);
    for (int i = 0; i < n; i++) {
        x[i] = nd(rng);
        gate[i] = nd(rng);
        up[i] = nd(rng);
    }

    std::vector<float> ref(n), got(n);
    float maxlogit = x[n - 1];
    softmax_ref(x.data(), n, maxlogit, ref.data());
    if (use_avx) nyaxus_softmax_avx(x.data(), n, maxlogit, got.data());
    else nyaxus_softmax_sse2(x.data(), n, maxlogit, got.data());
    check(max_abs(ref, got) < 1e-4f,
          (use_avx ? "AVX" : "SSE2") + std::string(" softmax n=") +
              std::to_string(n) + " max|err|=" + std::to_string(max_abs(ref, got)));

    swiglu_ref(gate.data(), up.data(), n, ref.data());
    if (use_avx) nyaxus_swiglu_avx(gate.data(), up.data(), n, got.data());
    else nyaxus_swiglu_sse2(gate.data(), up.data(), n, got.data());
    float err = max_abs(ref, got);
    // Relative-ish check: values can be large; compare against max|ref|.
    float scale = 0.0f;
    for (int i = 0; i < n; i++)
        if (std::fabs(ref[i]) > scale) scale = std::fabs(ref[i]);
    check(err < 1e-4f * (scale + 1.0f),
          (use_avx ? "AVX" : "SSE2") + std::string(" swiglu n=") +
              std::to_string(n) + " max|err|=" + std::to_string(err));
}

int main() {
    bool avx = nyaxus_cpu_has_avx_fma() != 0;
    std::printf("cpu has AVX+FMA: %d\n", (int)avx);
    // Needle sizes: exact multiples, tails, and small.
    for (int n : {8, 17, 64, 100, 250}) run_case(n, 1234u + (unsigned)n, avx);
    run_case(7, 4321, false);  // SSE2 tail, always callable
    run_case(1, 999, false);
    std::printf(g_fail ? "FAILURES: %d\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}