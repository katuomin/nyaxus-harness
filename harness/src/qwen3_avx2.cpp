// 256-bit AVX2 integer GEMM for the Qwen3 engine. Compiled with -mavx2
// (MSVC: /arch:AVX2) in its own target; only called after
// nyaxus_cpu_has_avx2() confirms the CPU supports AVX2.
//
// Y = (X_i16 · Wᵀ) ⊙ sx ⊙ scale
//   X_i16: [S,K] int16 - activations quantized per row (see
//           quantize_rows_i16 in qwen3.cpp): x_i16[k] = lrint(x/sx),
//           sx = max|x|/5461
//   sx:    [S] f32 per-row scale
//   W:     [N,K] int8 - the ORIGINAL safetensors layout (W[n*K + k..k+15])
//   scale: [N] f32 per-neuron weight scale
//   Y:     [S,N] f32
//
// The integer dot product is EXACT in int32 (no overflow): |x_i16| <= 5461,
// |w| <= 127, K <= 3072 -> |sum| <= 5461*127*3072 = 2.13e9 < 2^31. Both
// operands are signed, so vpmaddwd multiplies them directly - no +128 bias
// and no rowsum correction (unlike the earlier u8 scheme). The only rounding
// is the activation quantization and the final float dequantization, which
// matches the scalar reference (tests/i16i8_ref.cpp) operation-for-operation,
// making the two bit-identical on any AVX2 machine.
//
// Why int16 and not uint8: the u8 scheme kept X to 7 bits of mantissa
// (step = max|x|/127, ~0.9%/element). That ~3% per-GEMM error compounded
// across 28 layers and washed out the reranker's fine logit difference
// (relevant/irrelevant both collapsed to ~-0.7). 12.4 bits of mantissa
// (step = max|x|/5461) cuts the quantization error ~40x, restoring the
// signal while keeping 2x less weight traffic than the FP32 path.
#include <cstddef>
#include <cstdint>

#include <cstdlib>
#include <immintrin.h>

namespace {
// NYAXUS_NO_AVX2=1 forces the FP32 fallback (A/B testing, debugging).
int probe_cpu_avx2() {
    if (const char* e = std::getenv("NYAXUS_NO_AVX2"))
        if (e[0] == '1') return 0;
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    asm("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    if (eax < 7) return 0;
    asm("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    // Leaf 7, subleaf 0, EBX bit 5 = AVX2 (per GCC cpuid.h; verified
    // against __builtin_cpu_supports("avx2") on a Zen 3 host).
    return int((ebx >> 5) & 1u);
}
}  // namespace

extern "C" int nyaxus_cpu_has_avx2() {
    // Cache the result: this is consulted once at first dispatch.
    static const int cached = probe_cpu_avx2();
    return cached;
}

// Y[m][n] = (sum_k X_i16[m][k] * W[n*K + k]) * sx[m] * scale[n]
//
// b0..b1 are neuron indices. EIGHT neurons are processed per X load: one
// 256-bit X vector (16 int16 k-values) feeds all eight neurons' vpmaddwd,
// cutting the activation re-read traffic from 3.0 B/MAC (one neuron per X
// load) to 1.25 B/MAC. A L1-resident microbench on the target CPU shows the
// integer pipeline is fast (the full kernel runs at ~18% of the
// single-threaded micro rate, i.e. traffic-bound, not instruction-bound), so
// this blocking is where the INT8 speedup comes from.
//
// Loop order is neuron-block OUTER, token INNER: the 8 W rows of a block
// (8 KB) stay hot in L1 across all S tokens, so only X streams from L2/L3
// (~96 MB per GEMM at S=376). The reverse order (token outer) would stream
// all of W per token (~384 MB) - 4x more slow-memory traffic.
// Widen 16 int8 -> 16 int16, then vpmaddwd pairs adjacent int16×int16
// products into exact int32. No scratch is needed: accumulators are
// register-resident.
extern "C" void nyaxus_gemm_i16_i8_range_avx2(const int16_t* Xi16,
                                             const float* sx, int S, int K,
                                             const int8_t* W, const float* scale,
                                             int N, float* Y, int b0, int b1) {
    // Full 8-neuron blocks: branchless hot loop. (A per-neuron guard inside
    // the k-loop for the partial last block cost ~15% front-end throughput
    // on Zen 3 - every block pays six extra uops even when ncount == 8 -
    // so the partial remainder is handled scalar below instead.)
    for (int n0 = b0; n0 + 8 <= b1; n0 += 8) {
        for (int m = 0; m < S; m++) {
            const int16_t* xrow = Xi16 + static_cast<std::size_t>(m) * K;
            const float sxm = sx[m];
            __m256i acc0 = _mm256_setzero_si256();
            __m256i acc1 = _mm256_setzero_si256();
            __m256i acc2 = _mm256_setzero_si256();
            __m256i acc3 = _mm256_setzero_si256();
            __m256i acc4 = _mm256_setzero_si256();
            __m256i acc5 = _mm256_setzero_si256();
            __m256i acc6 = _mm256_setzero_si256();
            __m256i acc7 = _mm256_setzero_si256();
            int k = 0;
            for (; k + 16 <= K; k += 16) {
                // One X load feeds all eight neurons.
                __m256i x =
                    _mm256_loadu_si256(reinterpret_cast<const __m256i*>(xrow + k));
                __m256i w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 0) * K + k)));
                acc0 = _mm256_add_epi32(acc0, _mm256_madd_epi16(x, w));
                w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 1) * K + k)));
                acc1 = _mm256_add_epi32(acc1, _mm256_madd_epi16(x, w));
                w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 2) * K + k)));
                acc2 = _mm256_add_epi32(acc2, _mm256_madd_epi16(x, w));
                w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 3) * K + k)));
                acc3 = _mm256_add_epi32(acc3, _mm256_madd_epi16(x, w));
                w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 4) * K + k)));
                acc4 = _mm256_add_epi32(acc4, _mm256_madd_epi16(x, w));
                w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 5) * K + k)));
                acc5 = _mm256_add_epi32(acc5, _mm256_madd_epi16(x, w));
                w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 6) * K + k)));
                acc6 = _mm256_add_epi32(acc6, _mm256_madd_epi16(x, w));
                w = _mm256_cvtepi8_epi16(
                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(W + static_cast<std::size_t>(n0 + 7) * K + k)));
                acc7 = _mm256_add_epi32(acc7, _mm256_madd_epi16(x, w));
            }
            // Tail (K % 16 != 0): exact integer accumulation; integer sums
            // are order-exact, so adding it in changes nothing.
            for (int i = 0; i < 8; i++) {
                const int8_t* wrow = W + static_cast<std::size_t>(n0 + i) * K;
                int32_t total = 0;
                for (int kt = k; kt < K; kt++)
                    total += static_cast<int32_t>(xrow[kt]) * static_cast<int32_t>(wrow[kt]);
                int32_t accs[8];
                __m256i acc = (i == 0) ? acc0 : (i == 1) ? acc1 : (i == 2) ? acc2 :
                             (i == 3) ? acc3 : (i == 4) ? acc4 : (i == 5) ? acc5 :
                             (i == 6) ? acc6 : acc7;
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(accs), acc);
                for (int j = 0; j < 8; j++) total += accs[j];
                // Dequantize: total · sx · scale (identical to the scalar ref).
                Y[static_cast<std::size_t>(m) * N + n0 + i] =
                    (static_cast<float>(total) * sxm) * scale[n0 + i];
            }
        }
    }
    // Partial remainder (0-7 neurons; only when the range size is not a
    // multiple of 8): scalar, exact, negligible cost - and it keeps the
    // SIMD loop above branch-free. (Unguarded SIMD reads of n0+2..n0+7
    // would run past the W allocation, which is why this is not handled
    // in the vector loop.)
    for (int n = b0 + ((b1 - b0) / 8) * 8; n < b1; n++) {
        for (int m = 0; m < S; m++) {
            const int16_t* xrow = Xi16 + static_cast<std::size_t>(m) * K;
            const int8_t* wrow = W + static_cast<std::size_t>(n) * K;
            int32_t total = 0;
            for (int k = 0; k < K; k++)
                total += static_cast<int32_t>(xrow[k]) * static_cast<int32_t>(wrow[k]);
            Y[static_cast<std::size_t>(m) * N + n] =
                (static_cast<float>(total) * sx[m]) * scale[n];
        }
    }
}

extern "C" void nyaxus_gemm_i16_i8_avx2(const int16_t* Xi16, const float* sx,
                                        int S, int K, const int8_t* W,
                                        const float* scale, int N, float* Y) {
    nyaxus_gemm_i16_i8_range_avx2(Xi16, sx, S, K, W, scale, N, Y, 0, N);
}
