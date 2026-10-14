// Microbenchmark: instruction throughput of the i16i8 GEMM inner loop vs the
// FP32 FMA loop, with all data L1-resident (X 4 KB, W 16 KB) so the
// measurement is pure instruction issue, not memory. Settles whether the
// i16i8 kernel is instruction-bound (multi-neuron blocking ~0% headroom) or
// memory-bound (multi-neuron blocking could recover the gap) on this CPU.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <immintrin.h>

int main(int argc, char** argv) {
    // Runtime-valued trip count: keeps the compiler from unrolling/folding
    // the constant-operand loops below.
    const long ITERS = (argc > 1) ? std::atol(argv[1]) : 200000;
    const int K = 256, S = 8, N = 64;
    std::vector<int16_t> X(static_cast<std::size_t>(S) * K, 3);
    std::vector<int8_t> W(static_cast<std::size_t>(N) * K, 3);
    const int16_t* xrow = X.data();
    const int8_t* wrow = W.data();

    // 1) Raw vpmaddwd + vpaddd, operands from L1 and changing per iteration
    //    (no hoisting): the integer-pipeline ceiling for 16 MACs.
    {
        __m256i acc = _mm256_setzero_si256();
        auto t0 = std::chrono::high_resolution_clock::now();
        for (long i = 0; i < ITERS; i++) {
            __m256i a =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow + (i % K)));
            __m256i b = _mm256_cvtepi8_epi16(_mm_loadu_si128(
                reinterpret_cast<const __m128i*>(wrow + (i % K))));
            acc = _mm256_add_epi32(acc, _mm256_madd_epi16(a, b));
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::printf("vpmaddwd+vpaddd (L1):    %8.1f GMAC/s  (16 MACs/iter)\n",
                    double(ITERS) * 16.0 / s / 1e9);
        int acc0[8];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc0), acc);
        std::printf("  acc sanity: %d\n", acc0[0]);
    }
    // 2) Full i16i8 inner loop as in nyaxus_gemm_i16_i8_range_avx2:
    //    X load (32B) + W load (16B) + vpmovsxbw + vpmaddwd + vpaddd per 16 MACs.
    {
        __m256i acc = _mm256_setzero_si256();
        auto t0 = std::chrono::high_resolution_clock::now();
        for (long i = 0; i < ITERS; i++)
            for (int k = 0; k + 16 <= K; k += 16) {
                __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow + k));
                __m256i w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(wrow + k)));
                acc = _mm256_add_epi32(acc, _mm256_madd_epi16(x, w));
            }
        auto t1 = std::chrono::high_resolution_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::printf("i16i8 inner loop (L1):  %8.1f GMAC/s  (%d MACs/iter)\n",
                    double(ITERS) * K / s / 1e9, K);
        int acc0[8];
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc0), acc);
        std::printf("  acc sanity: %d\n", acc0[0]);
    }
    // 3) FP32 FMA loop (the kernel it competes with): 1 FMA per 8 MACs.
    {
        std::vector<float> xf(K, 1.0f), wf(K, 1.0f);
        __m256 acc = _mm256_setzero_ps();
        auto t0 = std::chrono::high_resolution_clock::now();
        for (long i = 0; i < ITERS; i++)
            for (int k = 0; k + 8 <= K; k += 8) {
                __m256 x = _mm256_loadu_ps(xf.data() + k);
                __m256 w = _mm256_loadu_ps(wf.data() + k);
                acc = _mm256_fmadd_ps(x, w, acc);
            }
        auto t1 = std::chrono::high_resolution_clock::now();
        double s = std::chrono::duration<double>(t1 - t0).count();
        std::printf("fp32 fmadd loop (L1):   %8.1f GMAC/s  (%d MACs/iter)\n",
                    double(ITERS) * K / s / 1e9, K);
        float acc0[8];
        _mm256_storeu_ps(acc0, acc);
        std::printf("  acc sanity: %f\n", acc0[0]);
    }
    return 0;
}
