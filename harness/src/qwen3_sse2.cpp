// 128-bit SSE2 kernels for the Qwen3 engine. SSE2 is part of the x86-64
// baseline, so this TU is safe on every x86-64 CPU. Compiled as its own
// target so the AVX TU can carry its own -m flags.
//
// Weight layout: [N, K] row-major (the original safetensors layout); each
// kernel transposes a [BK, BN] tile into scratch once per K chunk - see
// the AVX kernel header for why the [K, N] layout was reverted.
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <emmintrin.h>

// y = W x, W row-major [out, in]. Single-token fast path.
extern "C" void nyaxus_matvec_sse2(const float* W, int out_dim, int in_dim,
                                   const float* x, float* y) {
    for (int i = 0; i < out_dim; i++) {
        const float* row = W + static_cast<std::size_t>(i) * in_dim;
        __m128 acc = _mm_setzero_ps();
        int j = 0;
        for (; j + 4 <= in_dim; j += 4) {
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(row + j),
                                             _mm_loadu_ps(x + j)));
        }
        float lane[4];
        _mm_storeu_ps(lane, acc);
        float total = (lane[0] + lane[1]) + (lane[2] + lane[3]);
        for (; j < in_dim; j++) total += row[j] * x[j];
        y[i] = total;
    }
}

// Y = X · Wᵀ,  X: [S,K] row-major, W: [N,K] row-major, Y: [S,N] row-major.
//
// Blocked GEMM over neuron blocks [b0, b1): 4 neurons (BN) per 128-bit FMA.
// Each weight element is read once and applied to every token.
//
// The W tile [bk, bn] is transposed into scratch once per K chunk
// (amortized over all S tokens) - see the AVX kernel header for why the
// [K, N] layout was reverted (L1 set aliasing at N=1024).
//
// The inner loop processes TWO tokens per iteration, sharing each W vector
// load across both tokens' multiply-adds (see nyaxus_gemm_range_avx for the
// load-port analysis). Per-token arithmetic is unchanged (same 4 independent
// accumulators, same reduction order), so results are bit-identical to the
// single-token loop.
//
// `scratch` must hold at least (BK*BN + S*BN) floats: the transposed tile
// [BK, BN] followed by the partial sums [S, BN].
extern "C" void nyaxus_gemm_range_sse2(const float* X, int S, int K,
                                       const float* W, int N, float* Y,
                                       float* scratch, int b0, int b1) {
    const int BN = 4;
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
                    // 4) to hide multiply/add latency.
                    __m128 a00 = _mm_setzero_ps(), a10 = _mm_setzero_ps();
                    __m128 a20 = _mm_setzero_ps(), a30 = _mm_setzero_ps();
                    __m128 a01 = _mm_setzero_ps(), a11 = _mm_setzero_ps();
                    __m128 a21 = _mm_setzero_ps(), a31 = _mm_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        __m128 w0 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN);
                        __m128 w1 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN);
                        __m128 w2 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN);
                        __m128 w3 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN);
                        a00 = _mm_add_ps(a00, _mm_mul_ps(_mm_set1_ps(x0[k + 0]), w0));
                        a01 = _mm_add_ps(a01, _mm_mul_ps(_mm_set1_ps(x1[k + 0]), w0));
                        a10 = _mm_add_ps(a10, _mm_mul_ps(_mm_set1_ps(x0[k + 1]), w1));
                        a11 = _mm_add_ps(a11, _mm_mul_ps(_mm_set1_ps(x1[k + 1]), w1));
                        a20 = _mm_add_ps(a20, _mm_mul_ps(_mm_set1_ps(x0[k + 2]), w2));
                        a21 = _mm_add_ps(a21, _mm_mul_ps(_mm_set1_ps(x1[k + 2]), w2));
                        a30 = _mm_add_ps(a30, _mm_mul_ps(_mm_set1_ps(x0[k + 3]), w3));
                        a31 = _mm_add_ps(a31, _mm_mul_ps(_mm_set1_ps(x1[k + 3]), w3));
                    }
                    __m128 acc0 = _mm_add_ps(_mm_add_ps(a00, a10),
                                             _mm_add_ps(a20, a30));
                    __m128 acc1 = _mm_add_ps(_mm_add_ps(a01, a11),
                                             _mm_add_ps(a21, a31));
                    for (; k < bk; k++) {
                        __m128 w = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN);
                        acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_set1_ps(x0[k]), w));
                        acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_set1_ps(x1[k]), w));
                    }
                    float lane[4];
                    _mm_storeu_ps(lane, acc0);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m) * bn + c] += lane[c];
                    _mm_storeu_ps(lane, acc1);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m + 1) * bn + c] += lane[c];
                }
                // Odd trailing token (single-token path, same arithmetic).
                if (S & 1) {
                    const float* xrow = X + static_cast<std::size_t>(S - 1) * K + k0;
                    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
                    __m128 a2 = _mm_setzero_ps(), a3 = _mm_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_set1_ps(xrow[k + 0]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN)));
                        a1 = _mm_add_ps(a1, _mm_mul_ps(_mm_set1_ps(xrow[k + 1]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN)));
                        a2 = _mm_add_ps(a2, _mm_mul_ps(_mm_set1_ps(xrow[k + 2]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN)));
                        a3 = _mm_add_ps(a3, _mm_mul_ps(_mm_set1_ps(xrow[k + 3]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN)));
                    }
                    __m128 acc = _mm_add_ps(_mm_add_ps(a0, a1), _mm_add_ps(a2, a3));
                    for (; k < bk; k++)
                        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(xrow[k]),
                                                         _mm_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN)));
                    float lane[4];
                    _mm_storeu_ps(lane, acc);
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
        for (int m = 0; m < S; m++)
            for (int c = 0; c < bn; c++)
                Y[static_cast<std::size_t>(m) * N + n0 + c] =
                    Cbuf[static_cast<std::size_t>(m) * bn + c];
    }
}

extern "C" void nyaxus_gemm_sse2(const float* X, int S, int K, const float* W,
                                 int N, float* Y, float* scratch) {
    nyaxus_gemm_range_sse2(X, S, K, W, N, Y, scratch, 0, (N + 3) / 4);
}

// dot = a·b over n elements.
extern "C" float nyaxus_dot_sse2(const float* a, const float* b, int n) {
    __m128 acc = _mm_setzero_ps();
    int j = 0;
    for (; j + 4 <= n; j += 4)
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(a + j), _mm_loadu_ps(b + j)));
    float lane[4];
    _mm_storeu_ps(lane, acc);
    float total = (lane[0] + lane[1]) + (lane[2] + lane[3]);
    for (; j < n; j++) total += a[j] * b[j];
    return total;
}

// y += a * x over n elements.
extern "C" void nyaxus_axpy_sse2(float* y, float a, const float* x, int n) {
    int j = 0;
    for (; j + 4 <= n; j += 4) {
        __m128 v = _mm_loadu_ps(x + j);
        __m128 yy = _mm_loadu_ps(y + j);
        _mm_storeu_ps(y + j, _mm_add_ps(yy, _mm_mul_ps(_mm_set1_ps(a), v)));
    }
    for (; j < n; j++) y[j] += a * x[j];
}

// ---- Attention softmax and SwiGLU activation (fast exp) ----
//
// Same algorithm as the AVX variant but on 128-bit lanes (SSE2 has no
// round_ps, so round-to-nearest uses the 1.5*2^23 magic-constant trick,
// which is exact while |y| < 2^22 - true for all inputs here).
static inline __m128 exp128(__m128 x) {
    __m128 y = _mm_mul_ps(x, _mm_set1_ps(1.4426950408889634f));  // log2(e)
    // Round y to nearest integer: y + 1.5*2^23 - 1.5*2^23. Exact only under
    // the default MXCSR rounding mode (round-to-nearest-even) - do not run
    // this path with a modified rounding mode.
    __m128 magic = _mm_set1_ps(12582912.0f);
    __m128 i = _mm_sub_ps(_mm_add_ps(y, magic), magic);
    __m128 f = _mm_sub_ps(y, i);
    __m128 p = _mm_add_ps(_mm_mul_ps(f, _mm_set1_ps(0.001333359650326f)),
                          _mm_set1_ps(0.009618137138674f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(0.055504108664822f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(0.240226506959101f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(0.693147180559945f));
    p = _mm_add_ps(_mm_mul_ps(p, f), _mm_set1_ps(1.0f));
    i = _mm_max_ps(i, _mm_set1_ps(-126.0f));
    i = _mm_min_ps(i, _mm_set1_ps(127.0f));
    __m128i ie = _mm_cvtps_epi32(i);
    ie = _mm_add_epi32(ie, _mm_set1_epi32(127));
    __m128 scale = _mm_castsi128_ps(_mm_slli_epi32(ie, 23));
    return _mm_mul_ps(p, scale);
}

// Softmax with maxlogit subtracted: out[i] = exp(x[i]-maxlogit)/sum.
extern "C" void nyaxus_softmax_sse2(const float* x, int n, float maxlogit,
                                    float* out) {
    __m128 sumv = _mm_setzero_ps();
    int j = 0;
    for (; j + 4 <= n; j += 4) {
        __m128 e = exp128(_mm_sub_ps(_mm_loadu_ps(x + j),
                                     _mm_set1_ps(maxlogit)));
        _mm_storeu_ps(out + j, e);
        sumv = _mm_add_ps(sumv, e);
    }
    float lane[4];
    _mm_storeu_ps(lane, sumv);
    float sum = (lane[0] + lane[1]) + (lane[2] + lane[3]);
    for (; j < n; j++) {
        float e = std::exp(x[j] - maxlogit);
        out[j] = e;
        sum += e;
    }
    __m128 inv = _mm_set1_ps(1.0f / sum);
    for (j = 0; j + 4 <= n; j += 4)
        _mm_storeu_ps(out + j, _mm_mul_ps(_mm_loadu_ps(out + j), inv));
    for (; j < n; j++) out[j] /= sum;
}

// SiLU: out[j] = gate[j] * sigmoid(gate[j]) * up[j], 1/(1+e) per element.
extern "C" void nyaxus_swiglu_sse2(const float* gate, const float* up, int n,
                                   float* out) {
    const __m128 one = _mm_set1_ps(1.0f);
    int j = 0;
    for (; j + 4 <= n; j += 4) {
        __m128 g = _mm_loadu_ps(gate + j);
        __m128 u = _mm_loadu_ps(up + j);
        __m128 e = exp128(_mm_sub_ps(_mm_setzero_ps(), g));
        __m128 sig = _mm_div_ps(one, _mm_add_ps(one, e));
        __m128 gest = _mm_mul_ps(g, sig);
        _mm_storeu_ps(out + j, _mm_mul_ps(gest, u));
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
// Y: [S,N] f32. Same blocked structure as nyaxus_gemm_range_sse2 (including
// the two-token inner loop); the W tile is 1 byte/elem (¼ the weight
// traffic). The int8 tile is converted to FP32 ONCE per K chunk (amortized
// over all S tokens). The per-row scale is applied ONCE per neuron block
// (after the full K reduction), not per K chunk - applying it per chunk is
// ~16% slower.
// `scratch` must hold at least (BK*BN + S*BN) floats: the converted tile
// [BK, BN] followed by the partial sums [S, BN].
extern "C" void nyaxus_gemm_i8_range_sse2(const float* X, int S, int K,
                                          const int8_t* W, const float* scale,
                                          int N, float* Y, float* scratch,
                                          int b0, int b1) {
    const int BN = 4;
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
                    __m128 a00 = _mm_setzero_ps(), a10 = _mm_setzero_ps();
                    __m128 a20 = _mm_setzero_ps(), a30 = _mm_setzero_ps();
                    __m128 a01 = _mm_setzero_ps(), a11 = _mm_setzero_ps();
                    __m128 a21 = _mm_setzero_ps(), a31 = _mm_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        __m128 w0 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN);
                        __m128 w1 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN);
                        __m128 w2 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN);
                        __m128 w3 = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN);
                        a00 = _mm_add_ps(a00, _mm_mul_ps(_mm_set1_ps(x0[k + 0]), w0));
                        a01 = _mm_add_ps(a01, _mm_mul_ps(_mm_set1_ps(x1[k + 0]), w0));
                        a10 = _mm_add_ps(a10, _mm_mul_ps(_mm_set1_ps(x0[k + 1]), w1));
                        a11 = _mm_add_ps(a11, _mm_mul_ps(_mm_set1_ps(x1[k + 1]), w1));
                        a20 = _mm_add_ps(a20, _mm_mul_ps(_mm_set1_ps(x0[k + 2]), w2));
                        a21 = _mm_add_ps(a21, _mm_mul_ps(_mm_set1_ps(x1[k + 2]), w2));
                        a30 = _mm_add_ps(a30, _mm_mul_ps(_mm_set1_ps(x0[k + 3]), w3));
                        a31 = _mm_add_ps(a31, _mm_mul_ps(_mm_set1_ps(x1[k + 3]), w3));
                    }
                    __m128 acc0 = _mm_add_ps(_mm_add_ps(a00, a10),
                                             _mm_add_ps(a20, a30));
                    __m128 acc1 = _mm_add_ps(_mm_add_ps(a01, a11),
                                             _mm_add_ps(a21, a31));
                    for (; k < bk; k++) {
                        __m128 w = _mm_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN);
                        acc0 = _mm_add_ps(acc0, _mm_mul_ps(_mm_set1_ps(x0[k]), w));
                        acc1 = _mm_add_ps(acc1, _mm_mul_ps(_mm_set1_ps(x1[k]), w));
                    }
                    float lane[4];
                    _mm_storeu_ps(lane, acc0);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m) * bn + c] += lane[c];
                    _mm_storeu_ps(lane, acc1);
                    for (int c = 0; c < BN; c++)
                        Cbuf[static_cast<std::size_t>(m + 1) * bn + c] += lane[c];
                }
                if (S & 1) {
                    const float* xrow = X + static_cast<std::size_t>(S - 1) * K + k0;
                    __m128 a0 = _mm_setzero_ps(), a1 = _mm_setzero_ps();
                    __m128 a2 = _mm_setzero_ps(), a3 = _mm_setzero_ps();
                    int k = 0;
                    for (; k + 4 <= bk; k += 4) {
                        a0 = _mm_add_ps(a0, _mm_mul_ps(_mm_set1_ps(xrow[k + 0]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 0) * BN)));
                        a1 = _mm_add_ps(a1, _mm_mul_ps(_mm_set1_ps(xrow[k + 1]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 1) * BN)));
                        a2 = _mm_add_ps(a2, _mm_mul_ps(_mm_set1_ps(xrow[k + 2]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 2) * BN)));
                        a3 = _mm_add_ps(a3, _mm_mul_ps(_mm_set1_ps(xrow[k + 3]),
                                                       _mm_loadu_ps(Wtf + static_cast<std::size_t>(k + 3) * BN)));
                    }
                    __m128 acc = _mm_add_ps(_mm_add_ps(a0, a1), _mm_add_ps(a2, a3));
                    for (; k < bk; k++)
                        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(xrow[k]),
                                                         _mm_loadu_ps(Wtf + static_cast<std::size_t>(k) * BN)));
                    float lane[4];
                    _mm_storeu_ps(lane, acc);
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

extern "C" void nyaxus_gemm_i8_sse2(const float* X, int S, int K, const int8_t* W,
                                    const float* scale, int N, float* Y,
                                    float* scratch) {
    nyaxus_gemm_i8_range_sse2(X, S, K, W, scale, N, Y, scratch, 0, (N + 3) / 4);
}
