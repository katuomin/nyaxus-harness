// 256-bit AVX+FMA kernels for the Qwen3 engine. Compiled with -mavx -mfma in
// its own target; only called after nyaxus_cpu_has_avx_fma() confirms the CPU
// supports both AVX and FMA.
//
// Note: these use only AVX1 (256-bit floating point) + FMA instructions. They
// do NOT require AVX2 - AVX2 adds 256-bit *integer* ops, which a pure FP32
// matmul never uses. So the same code runs at the same speed on CPUs with or
// without AVX2, and it is fully testable on any AVX+FMA machine.
//
// Weight layout: [N, K] row-major (the original safetensors layout), so
// W[n*K + k] is the k-th input of neuron n. Each kernel transposes a
// [BK, BN] tile into scratch once per K chunk (amortized over all S
// tokens); the [K, N] layout was tried and reverted - with N a power of
// two it aliases a single L1 set and every W load misses.
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <immintrin.h>

namespace {
int probe_cpu_avx_fma() {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    asm("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    bool avx = (ecx >> 28) & 1u;  // ECX bit 28 = AVX
    bool fma = (ecx >> 12) & 1u;  // ECX bit 12 = FMA
    return (avx && fma) ? 1 : 0;
}
}  // namespace

extern "C" int nyaxus_cpu_has_avx_fma() {
    // Cache the cpuid result: this is consulted once at first dispatch.
    static const int cached = probe_cpu_avx_fma();
    return cached;
}

// y = W x, W row-major [out, in]. Single-token fast path.
extern "C" void nyaxus_matvec_avx(const float* W, int out_dim, int in_dim,
                                  const float* x, float* y) {
    for (int i = 0; i < out_dim; i++) {
        const float* row = W + static_cast<std::size_t>(i) * in_dim;
        __m256 acc = _mm256_setzero_ps();
        int j = 0;
        for (; j + 8 <= in_dim; j += 8) {
            acc = _mm256_fmadd_ps(_mm256_loadu_ps(row + j),
                                  _mm256_loadu_ps(x + j), acc);
        }
        float lane[8];
        _mm256_storeu_ps(lane, acc);
        float total = 0.0f;
        for (int k = 0; k < 8; k++) total += lane[k];
        for (; j < in_dim; j++) total += row[j] * x[j];
        y[i] = total;
    }
}

// Y = X · Wᵀ,  X: [S,K] row-major, W: [N,K] row-major, Y: [S,N] row-major.
//
// Blocked GEMM over neuron blocks [b0, b1): 8 neurons (BN) per 256-bit FMA.
// Each weight element is read once and applied to every token.
//
// The W tile [bk, bn] is transposed into scratch once per K chunk
// (amortized over all S tokens). A [K, N] weight layout was tried and
// reverted: with N=1024 every k-row maps to the same L1 set
// ((k*4096/64) % 64 == 0) and the W stream thrashes L1 - measured 2.4x
// slower on host.
//
// The inner loop processes TWO tokens per iteration, sharing each W vector
// load across both tokens' FMAs. One token per iteration is 2 loads per FMA
// (scalar broadcast + W load), which saturates a 2-load-port core at ~50% of
// FMA peak; two tokens is 1.5 loads per FMA, lifting that ceiling. Per-token
// arithmetic is unchanged (same 4 independent accumulators, same reduction
// order), so results are bit-identical to the single-token loop.
//
// `scratch` must hold at least (BK*BN + S*BN) floats: the transposed tile
// [BK, BN] followed by the partial sums [S, BN].
extern "C" void nyaxus_gemm_range_avx(const float* X, int S, int K,
                                      const float* W, int N, float* Y,
                                      float* scratch, int b0, int b1) {
    const int BN = 8;
    const int BK = 64;
    float* Wtf = scratch;            // transposed W tile: [BK, BN]
    float* Cbuf = scratch + BK * BN; // partial sums: [S, BN]
    for (int nb = b0; nb < b1; nb++) {
        int n0 = nb * BN;
        int bn = (n0 + BN <= N) ? BN : N - n0;
        for (int m = 0; m < S; m++)
            for (int c = 0; c < bn; c++)
                Cbuf[static_cast<std::size_t>(m) * bn + c] = 0.0f;
        for (int k0 = 0; k0 < K; k0 += BK) {
            int bk = (k0 + BK <= K) ? BK : K - k0;
            // Transpose the W tile [bn, bk] from [N, K] into [bk, bn].
            for (int c = 0; c < bn; c++)
                for (int k = 0; k < bk; k++)
                    Wtf[static_cast<std::size_t>(k) * bn + c] =
                        W[static_cast<std::size_t>(n0 + c) * K + k0 + k];
            if (bn == BN) {
                // Two tokens per iteration; each W load feeds both tokens.
                for (int m = 0; m + 1 < S; m += 2) {
                    const float* x0 = X + static_cast<std::size_t>(m) * K + k0;
                    const float* x1 = X + static_cast<std::size_t>(m + 1) * K + k0;
                    // Four independent accumulators per token (k unrolled by
                    // 4): a single dependent FMA chain caps throughput at
                    // ~1/FMA-latency; four chains let the OoO core issue
                    // 2 FMA/cycle.
                    __m256 a00 = _mm256_setzero_ps(), a10 = _mm256_setzero_ps();
                    __m256 a20 = _mm256_setzero_ps(), a30 = _mm256_setzero_ps();
                    __m256 a01 = _mm256_setzero_ps(), a11 = _mm256_setzero_ps();
                    __m256 a21 = _mm256_setzero_ps(), a31 = _mm256_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        __m256 w0 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN);
                        __m256 w1 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN);
                        __m256 w2 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN);
                        __m256 w3 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN);
                        a00 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 0), w0, a00);
                        a01 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 0), w0, a01);
                        a10 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 1), w1, a10);
                        a11 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 1), w1, a11);
                        a20 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 2), w2, a20);
                        a21 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 2), w2, a21);
                        a30 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 3), w3, a30);
                        a31 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 3), w3, a31);
                    }
                    __m256 acc0 = _mm256_add_ps(_mm256_add_ps(a00, a10),
                                                _mm256_add_ps(a20, a30));
                    __m256 acc1 = _mm256_add_ps(_mm256_add_ps(a01, a11),
                                                _mm256_add_ps(a21, a31));
                    for (; k < bk; k++) {
                        __m256 w = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN);
                        acc0 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k), w, acc0);
                        acc1 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k), w, acc1);
                    }
                    float lane[8];
                    _mm256_storeu_ps(lane, acc0);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m) * bn + c] += lane[c];
                    _mm256_storeu_ps(lane, acc1);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m + 1) * bn + c] += lane[c];
                }
                // Odd trailing token (single-token path, same arithmetic).
                if (S & 1) {
                    const float* xrow = X + static_cast<std::size_t>(S - 1) * K + k0;
                    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
                    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        a0 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 0),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN), a0);
                        a1 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 1),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN), a1);
                        a2 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 2),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN), a2);
                        a3 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 3),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN), a3);
                    }
                    __m256 acc = _mm256_add_ps(_mm256_add_ps(a0, a1),
                                               _mm256_add_ps(a2, a3));
                    for (; k < bk; k++)
                        acc = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k),
                                              _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN), acc);
                    float lane[8];
                    _mm256_storeu_ps(lane, acc);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(S - 1) * bn + c] += lane[c];
                }
            } else {
                // Tail block (bn < BN): scalar per neuron.
                for (int m = 0; m < S; m++) {
                    const float* xrow = X + static_cast<std::size_t>(m) * K + k0;
                    for (int c = 0; c < bn; c++) {
                        float acc = 0.0f;
                        for (int k = 0; k < bk; k++)
                            acc += xrow[k] * Wtf[static_cast<std::size_t>(k) * bn + c];
                        Cbuf[static_cast<std::size_t>(m) * bn + c] += acc;
                    }
                }
            }
        }
        for (int m = 0; m < S; m++)
            for (int c = 0; c < bn; c++)
                Y[static_cast<std::size_t>(m) * N + n0 + c] =
                    Cbuf[static_cast<std::size_t>(m) * bn + c];
    }
}

extern "C" void nyaxus_gemm_avx(const float* X, int S, int K, const float* W,
                                int N, float* Y, float* scratch) {
    nyaxus_gemm_range_avx(X, S, K, W, N, Y, scratch, 0, (N + 7) / 8);
}

// dot = a·b over n elements.
extern "C" float nyaxus_dot_avx(const float* a, const float* b, int n) {
    __m256 acc = _mm256_setzero_ps();
    int j = 0;
    for (; j + 8 <= n; j += 8)
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(a + j), _mm256_loadu_ps(b + j), acc);
    float lane[8];
    _mm256_storeu_ps(lane, acc);
    float total = 0.0f;
    for (int k = 0; k < 8; k++) total += lane[k];
    for (; j < n; j++) total += a[j] * b[j];
    return total;
}

// y += a * x over n elements.
extern "C" void nyaxus_axpy_avx(float* y, float a, const float* x, int n) {
    int j = 0;
    for (; j + 8 <= n; j += 8) {
        __m256 v = _mm256_loadu_ps(x + j);
        __m256 yy = _mm256_loadu_ps(y + j);
        _mm256_storeu_ps(y + j, _mm256_fmadd_ps(_mm256_set1_ps(a), v, yy));
    }
    for (; j < n; j++) y[j] += a * x[j];
}

// ---- Attention softmax and SwiGLU activation (fast exp) ----
//
// exp(x) = 2^(x*log2e): split y = x*log2e into integer i + fraction f in
// [-0.5, 0.5], evaluate 2^f with a degree-5 Taylor poly (max |err| ~ 2.4e-6
// on [-0.5, 0.5] - plenty for attention weights), and scale by 2^i via the
// exponent field. The 256-bit integer shifts are AVX2, so the exponent is
// built on 128-bit lanes and recombined.
static inline __m256 exp256(__m256 x) {
    __m256 y = _mm256_mul_ps(x, _mm256_set1_ps(1.4426950408889634f));  // log2(e)
    __m256 i = _mm256_round_ps(y, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 f = _mm256_sub_ps(y, i);
    // 2^f via Horner: 1 + f(c1 + f(c2 + f(c3 + f(c4 + f*c5))))
    __m256 p = _mm256_fmadd_ps(f, _mm256_set1_ps(0.001333359650326f),
                               _mm256_set1_ps(0.009618137138674f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.055504108664822f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.240226506959101f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(0.693147180559945f));
    p = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.0f));
    // Clamp i so the exponent stays a normal float (softmax inputs are <= 0,
    // so this only bounds underflow; sigmoid input -g can be positive).
    i = _mm256_max_ps(i, _mm256_set1_ps(-126.0f));
    i = _mm256_min_ps(i, _mm256_set1_ps(127.0f));
    __m256i i256 = _mm256_cvtps_epi32(i);
    __m128i ilo = _mm256_castsi256_si128(i256);
    __m128i ihi = _mm256_extractf128_si256(i256, 1);
    __m128i e127 = _mm_set1_epi32(127);
    __m128i slo = _mm_slli_epi32(_mm_add_epi32(ilo, e127), 23);
    __m128i shi = _mm_slli_epi32(_mm_add_epi32(ihi, e127), 23);
    __m256 scale = _mm256_castsi256_ps(_mm256_insertf128_si256(
        _mm256_castsi128_si256(slo), shi, 1));
    return _mm256_mul_ps(p, scale);
}

// Softmax with maxlogit subtracted: out[i] = exp(x[i]-maxlogit)/sum. In-place
// allowed. The unnormalized exponentials are computed SIMD-wide, the scalar
// tail uses std::exp (correctly rounded, so the ~7 leftover elements are
// exact; the SIMD body is within 3e-6 relative).
extern "C" void nyaxus_softmax_avx(const float* x, int n, float maxlogit,
                                    float* out) {
    __m256 sumv = _mm256_setzero_ps();
    int j = 0;
    for (; j + 8 <= n; j += 8) {
        __m256 e = exp256(_mm256_sub_ps(_mm256_loadu_ps(x + j),
                                        _mm256_set1_ps(maxlogit)));
        _mm256_storeu_ps(out + j, e);
        sumv = _mm256_add_ps(sumv, e);
    }
    float lane[8];
    _mm256_storeu_ps(lane, sumv);
    float sum = (lane[0] + lane[1]) + (lane[2] + lane[3]) +
                (lane[4] + lane[5]) + (lane[6] + lane[7]);
    for (; j < n; j++) {
        float e = std::exp(x[j] - maxlogit);
        out[j] = e;
        sum += e;
    }
    __m256 inv = _mm256_set1_ps(1.0f / sum);
    for (j = 0; j + 8 <= n; j += 8)
        _mm256_storeu_ps(out + j,
                         _mm256_mul_ps(_mm256_loadu_ps(out + j), inv));
    for (; j < n; j++) out[j] /= sum;
}

// SiLU: out[j] = gate[j] * sigmoid(gate[j]) * up[j], 1/(1+e) per element.
extern "C" void nyaxus_swiglu_avx(const float* gate, const float* up, int n,
                                  float* out) {
    const __m256 one = _mm256_set1_ps(1.0f);
    int j = 0;
    for (; j + 8 <= n; j += 8) {
        __m256 g = _mm256_loadu_ps(gate + j);
        __m256 u = _mm256_loadu_ps(up + j);
        __m256 e = exp256(_mm256_sub_ps(_mm256_setzero_ps(), g));
        __m256 sig = _mm256_div_ps(one, _mm256_add_ps(one, e));
        __m256 gest = _mm256_mul_ps(g, sig);
        _mm256_storeu_ps(out + j, _mm256_mul_ps(gest, u));
    }
    for (; j < n; j++) {
        float g = gate[j];
        float sig = 1.0f / (1.0f + std::exp(-g));
        out[j] = g * sig * up[j];
    }
}

// ---- INT8 weight GEMM (FP32 accumulate) ----
//
// Y = (X · Wᵀ) ⊙ scale,  X: [S,K] f32, W: [N,K] int8 (original layout -
// the AVX2 integer kernel wants neurons contiguous along k), scale: [N] f32,
// Y: [S,N] f32. Same blocked structure as nyaxus_gemm_range_avx (including
// the two-token inner loop); the W tile is 1 byte/elem (¼ the weight
// traffic). The int8 tile is converted to FP32 ONCE per K chunk (amortized
// over all S tokens). The per-row scale is applied ONCE per neuron block
// (after the full K reduction), not per K chunk - applying it per chunk was
// measured ~16% slower.
// `scratch` must hold at least (BK*BN + S*BN) floats: the converted tile
// [BK, BN] followed by the partial sums [S, BN].
extern "C" void nyaxus_gemm_i8_range_avx(const float* X, int S, int K,
                                         const int8_t* W, const float* scale,
                                         int N, float* Y, float* scratch,
                                         int b0, int b1) {
    const int BN = 8;
    const int BK = 64;
    float* Wtf = scratch;            // W tile converted to f32: [BK, BN]
    float* Cbuf = scratch + BK * BN; // unscaled partial sums: [S, BN]
    for (int nb = b0; nb < b1; nb++) {
        int n0 = nb * BN;
        int bn = (n0 + BN <= N) ? BN : N - n0;
        for (int m = 0; m < S; m++)
            for (int c = 0; c < bn; c++)
                Cbuf[static_cast<std::size_t>(m) * bn + c] = 0.0f;
        for (int k0 = 0; k0 < K; k0 += BK) {
            int bk = (k0 + BK <= K) ? BK : K - k0;
            // Convert the W tile [bn, bk] to FP32 ([N,K] source layout).
            for (int c = 0; c < bn; c++)
                for (int k = 0; k < bk; k++)
                    Wtf[static_cast<std::size_t>(k) * bn + c] =
                        float(W[static_cast<std::size_t>(n0 + c) * K + k0 + k]);
            if (bn == BN) {
                // Two tokens per iteration; each W load feeds both tokens.
                for (int m = 0; m + 1 < S; m += 2) {
                    const float* x0 = X + static_cast<std::size_t>(m) * K + k0;
                    const float* x1 = X + static_cast<std::size_t>(m + 1) * K + k0;
                    __m256 a00 = _mm256_setzero_ps(), a10 = _mm256_setzero_ps();
                    __m256 a20 = _mm256_setzero_ps(), a30 = _mm256_setzero_ps();
                    __m256 a01 = _mm256_setzero_ps(), a11 = _mm256_setzero_ps();
                    __m256 a21 = _mm256_setzero_ps(), a31 = _mm256_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        __m256 w0 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN);
                        __m256 w1 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN);
                        __m256 w2 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN);
                        __m256 w3 = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN);
                        a00 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 0), w0, a00);
                        a01 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 0), w0, a01);
                        a10 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 1), w1, a10);
                        a11 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 1), w1, a11);
                        a20 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 2), w2, a20);
                        a21 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 2), w2, a21);
                        a30 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k + 3), w3, a30);
                        a31 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k + 3), w3, a31);
                    }
                    __m256 acc0 = _mm256_add_ps(_mm256_add_ps(a00, a10),
                                                _mm256_add_ps(a20, a30));
                    __m256 acc1 = _mm256_add_ps(_mm256_add_ps(a01, a11),
                                                _mm256_add_ps(a21, a31));
                    for (; k < bk; k++) {
                        __m256 w = _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN);
                        acc0 = _mm256_fmadd_ps(_mm256_broadcast_ss(x0 + k), w, acc0);
                        acc1 = _mm256_fmadd_ps(_mm256_broadcast_ss(x1 + k), w, acc1);
                    }
                    float lane[8];
                    _mm256_storeu_ps(lane, acc0);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m) * bn + c] += lane[c];
                    _mm256_storeu_ps(lane, acc1);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m + 1) * bn + c] += lane[c];
                }
                if (S & 1) {
                    const float* xrow = X + static_cast<std::size_t>(S - 1) * K + k0;
                    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
                    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        a0 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 0),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN), a0);
                        a1 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 1),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN), a1);
                        a2 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 2),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN), a2);
                        a3 = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k + 3),
                                             _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN), a3);
                    }
                    __m256 acc = _mm256_add_ps(_mm256_add_ps(a0, a1),
                                               _mm256_add_ps(a2, a3));
                    for (; k < bk; k++)
                        acc = _mm256_fmadd_ps(_mm256_broadcast_ss(xrow + k),
                                              _mm256_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN), acc);
                    float lane[8];
                    _mm256_storeu_ps(lane, acc);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(S - 1) * bn + c] += lane[c];
                }
            } else {
                for (int m = 0; m < S; m++) {
                    const float* xrow = X + static_cast<std::size_t>(m) * K + k0;
                    for (int c = 0; c < bn; c++) {
                        float acc = 0.0f;
                        for (int k = 0; k < bk; k++)
                            acc += xrow[k] * Wtf[static_cast<std::size_t>(k) * bn + c];
                        Cbuf[static_cast<std::size_t>(m) * bn + c] += acc;
                    }
                }
            }
        }
        // Apply the per-row scale once, after the full K reduction.
        for (int m = 0; m < S; m++)
            for (int c = 0; c < bn; c++)
                Y[static_cast<std::size_t>(m) * N + n0 + c] =
                    Cbuf[static_cast<std::size_t>(m) * bn + c] * scale[n0 + c];
    }
}

extern "C" void nyaxus_gemm_i8_avx(const float* X, int S, int K, const int8_t* W,
                                   const float* scale, int N, float* Y,
                                   float* scratch) {
    nyaxus_gemm_i8_range_avx(X, S, K, W, scale, N, Y, scratch, 0, (N + 7) / 8);
}
