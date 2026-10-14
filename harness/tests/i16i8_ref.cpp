// Unit test for the AVX2 int16×int8 GEMM kernel (src/qwen3_avx2.cpp).
//
// The kernel's integer dot product is exact (int32 accumulation, no
// overflow), and its dequantization expression matches the scalar reference
// below operation-for-operation, so on AVX2 machines the SIMD output must be
// bit-identical to the scalar reference. This test also validates the
// quantization + integer algorithm against a naive FP32 matmul of the
// original (unquantized) data.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

extern "C" int nyaxus_cpu_has_avx2();
extern "C" void nyaxus_gemm_i16_i8_avx2(const int16_t* Xi16, const float* sx,
                                        int S, int K, const int8_t* W,
                                        const float* scale, int N, float* Y);

static int g_fail = 0;
static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "  PASS  " : "  FAIL  ", what.c_str());
    if (!cond) g_fail++;
}

// Mirrors quantize_rows_i16 in qwen3.cpp (identical expressions, so the two
// stay bit-identical).
static void quantize(const float* X, int S, int K, std::vector<int16_t>& xi16,
                     std::vector<float>& sx) {
    xi16.resize(static_cast<std::size_t>(S) * K);
    sx.resize(S);
    for (int m = 0; m < S; m++) {
        const float* xrow = X + static_cast<std::size_t>(m) * K;
        float max_abs = 0.0f;
        for (int k = 0; k < K; k++) {
            float a = std::fabs(xrow[k]);
            if (a > max_abs) max_abs = a;
        }
        float s = max_abs / 5461.0f;
        sx[m] = s;
        int16_t* out = xi16.data() + static_cast<std::size_t>(m) * K;
        if (s == 0.0f) {
            for (int k = 0; k < K; k++) out[k] = 0;
            continue;
        }
        for (int k = 0; k < K; k++)
            out[k] = static_cast<int16_t>(std::lrintf(xrow[k] / s));
    }
}

// Scalar reference: same algorithm and dequantization expression as the
// SIMD kernel.
static void gemm_ref(const int16_t* xi16, const float* sx, int S, int K,
                     const int8_t* W, const float* scale, int N, float* Y) {
    for (int m = 0; m < S; m++)
        for (int n = 0; n < N; n++) {
            int32_t acc = 0;
            for (int k = 0; k < K; k++)
                acc += static_cast<int32_t>(xi16[static_cast<std::size_t>(m) * K + k]) *
                       static_cast<int32_t>(W[static_cast<std::size_t>(n) * K + k]);
            Y[static_cast<std::size_t>(m) * N + n] =
                (static_cast<float>(acc) * sx[m]) * scale[n];
        }
}

// Naive FP32 matmul of the original (unquantized) X against the dequantized
// W: the ground truth the quantized path approximates.
static void gemm_naive(const float* X, int S, int K, const int8_t* W,
                       const float* scale, int N, float* Y) {
    for (int m = 0; m < S; m++)
        for (int n = 0; n < N; n++) {
            float acc = 0.0f;
            for (int k = 0; k < K; k++)
                acc += X[static_cast<std::size_t>(m) * K + k] *
                       (static_cast<float>(W[static_cast<std::size_t>(n) * K + k]) * scale[n]);
            Y[static_cast<std::size_t>(m) * N + n] = acc;
        }
}

static double cosine(const float* a, const float* b, int n) {
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < n; i++) {
        dot += double(a[i]) * double(b[i]);
        na += double(a[i]) * double(a[i]);
        nb += double(b[i]) * double(b[i]);
    }
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

static void run_case(int S, int K, int N, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> nd(0.0f, 1.0f);
    std::uniform_int_distribution<int> wi(-100, 100);
    std::vector<float> X(static_cast<std::size_t>(S) * K);
    for (auto& v : X) v = nd(rng);
    std::vector<int8_t> W(static_cast<std::size_t>(N) * K);
    for (auto& v : W) v = static_cast<int8_t>(wi(rng));
    std::vector<float> scale(N);
    for (int n = 0; n < N; n++) scale[n] = 0.01f * (1 + (n % 7));

    std::vector<int16_t> xi16;
    std::vector<float> sx;
    quantize(X.data(), S, K, xi16, sx);

    std::vector<float> Yref(static_cast<std::size_t>(S) * N),
        Ynaive(static_cast<std::size_t>(S) * N);
    gemm_ref(xi16.data(), sx.data(), S, K, W.data(), scale.data(), N,
             Yref.data());
    gemm_naive(X.data(), S, K, W.data(), scale.data(), N, Ynaive.data());

    double c = cosine(Yref.data(), Ynaive.data(), S * N);
    check(c > 0.999, "S=" + std::to_string(S) + " K=" + std::to_string(K) +
                        " N=" + std::to_string(N) +
                        ": scalar ref cosine vs naive FP32 = " +
                        std::to_string(c));

    if (nyaxus_cpu_has_avx2()) {
        std::vector<float> Ysimd(static_cast<std::size_t>(S) * N);
        nyaxus_gemm_i16_i8_avx2(xi16.data(), sx.data(), S, K, W.data(),
                                scale.data(), N, Ysimd.data());
        bool exact = true;
        for (std::size_t i = 0; i < Ysimd.size(); i++)
            if (Ysimd[i] != Yref[i]) {
                exact = false;
                break;
            }
        check(exact, "S=" + std::to_string(S) + " K=" + std::to_string(K) +
                        " N=" + std::to_string(N) +
                        ": AVX2 kernel bit-identical to scalar reference");
    }
}

int main() {
    std::printf("AVX2: %d\n", nyaxus_cpu_has_avx2());
    run_case(3, 64, 48, 12345);      // K, N divisible by 16
    run_case(1, 100, 32, 67890);     // K tail (100 % 16 = 4)
    run_case(5, 3072, 1024, 24680);  // model-sized K, N
    run_case(4, 96, 50, 24681);      // N tail: last block ncount=2 (no guards)
    run_case(4, 96, 57, 24682);      // N tail: last block ncount=7 (all guards)
    std::printf(g_fail ? "FAILURES: %d\n" : "ALL PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
