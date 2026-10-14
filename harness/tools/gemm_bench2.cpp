// Variant experiments for the INT8 GEMM kernel: isolate the cost of the
// int8->f32 tile conversion vs the per-neuron scale multiply.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <immintrin.h>

extern "C" int nyaxus_cpu_has_avx_fma();

// A: conversion + scale (as in qwen3_avx.cpp)
extern "C" void gemm_i8_full(const float* X, int S, int K, const int8_t* W,
                             const float* scale, int N, float* Y,
                             float* scratch) {
    const int BN = 8, BK = 64;
    float* Wtf = scratch;
    float* Cbuf = scratch + BK * BN;
    for (int n0 = 0; n0 < N; n0 += BN) {
        int bn = BN;
        for (int m = 0; m < S; m++)
            for (int nb = 0; nb < bn; nb++) Cbuf[m * bn + nb] = 0.0f;
        for (int k0 = 0; k0 < K; k0 += BK) {
            for (int nb = 0; nb < bn; nb++)
                for (int k = 0; k < BK; k++)
                    Wtf[k * bn + nb] = float(W[(n0 + nb) * K + k0 + k]);
            for (int m = 0; m < S; m++) {
                const float* xrow = X + m * K + k0;
                __m256 acc = _mm256_setzero_ps();
                for (int k = 0; k < BK; k++)
                    acc = _mm256_fmadd_ps(_mm256_set1_ps(xrow[k]),
                                          _mm256_loadu_ps(Wtf + k * BN), acc);
                float lane[8];
                _mm256_storeu_ps(lane, acc);
                for (int nb = 0; nb < BN; nb++)
                    Cbuf[m * bn + nb] += lane[nb] * scale[n0 + nb];
            }
        }
        for (int m = 0; m < S; m++)
            for (int nb = 0; nb < bn; nb++) Y[m * N + n0 + nb] = Cbuf[m * bn + nb];
    }
}

// B: conversion, NO scale multiply
extern "C" void gemm_i8_noscale(const float* X, int S, int K, const int8_t* W,
                                const float* scale, int N, float* Y,
                                float* scratch) {
    (void)scale;
    const int BN = 8, BK = 64;
    float* Wtf = scratch;
    float* Cbuf = scratch + BK * BN;
    for (int n0 = 0; n0 < N; n0 += BN) {
        int bn = BN;
        for (int m = 0; m < S; m++)
            for (int nb = 0; nb < bn; nb++) Cbuf[m * bn + nb] = 0.0f;
        for (int k0 = 0; k0 < K; k0 += BK) {
            for (int nb = 0; nb < bn; nb++)
                for (int k = 0; k < BK; k++)
                    Wtf[k * bn + nb] = float(W[(n0 + nb) * K + k0 + k]);
            for (int m = 0; m < S; m++) {
                const float* xrow = X + m * K + k0;
                __m256 acc = _mm256_setzero_ps();
                for (int k = 0; k < BK; k++)
                    acc = _mm256_fmadd_ps(_mm256_set1_ps(xrow[k]),
                                          _mm256_loadu_ps(Wtf + k * BN), acc);
                float lane[8];
                _mm256_storeu_ps(lane, acc);
                for (int nb = 0; nb < BN; nb++) Cbuf[m * bn + nb] += lane[nb];
            }
        }
        for (int m = 0; m < S; m++)
            for (int nb = 0; nb < bn; nb++) Y[m * N + n0 + nb] = Cbuf[m * bn + nb];
    }
}

// C: NO conversion (W already f32) + scale - isolates scale cost vs f32 kernel
extern "C" void gemm_f32_scale(const float* X, int S, int K, const float* W,
                               const float* scale, int N, float* Y,
                               float* scratch) {
    const int BN = 8, BK = 64;
    float* Wt = scratch;
    float* Cbuf = scratch + BK * BN;
    for (int n0 = 0; n0 < N; n0 += BN) {
        int bn = BN;
        for (int m = 0; m < S; m++)
            for (int nb = 0; nb < bn; nb++) Cbuf[m * bn + nb] = 0.0f;
        for (int k0 = 0; k0 < K; k0 += BK) {
            for (int nb = 0; nb < bn; nb++)
                for (int k = 0; k < BK; k++)
                    Wt[k * bn + nb] = W[(n0 + nb) * K + k0 + k];
            for (int m = 0; m < S; m++) {
                const float* xrow = X + m * K + k0;
                __m256 acc = _mm256_setzero_ps();
                for (int k = 0; k < BK; k++)
                    acc = _mm256_fmadd_ps(_mm256_set1_ps(xrow[k]),
                                          _mm256_loadu_ps(Wt + k * BN), acc);
                float lane[8];
                _mm256_storeu_ps(lane, acc);
                for (int nb = 0; nb < BN; nb++)
                    Cbuf[m * bn + nb] += lane[nb] * scale[n0 + nb];
            }
        }
        for (int m = 0; m < S; m++)
            for (int nb = 0; nb < bn; nb++) Y[m * N + n0 + nb] = Cbuf[m * bn + nb];
    }
}

int main() {
    const int S = 376, K = 1024, N = 2048;
    std::vector<float> X(size_t(S) * K), Wf(size_t(N) * K), Y(size_t(S) * N);
    std::vector<float> scratch(size_t(K) * 8 + size_t(S) * 8);
    std::vector<int8_t> Wi(size_t(N) * K);
    std::vector<float> scale(N, 1.0f);
    srand(1);
    for (auto& v : X) v = float(rand()) / 30000.0f - 0.5f;
    for (auto& v : Wf) v = float(rand()) / 30000.0f - 0.5f;
    for (auto& v : Wi) v = int8_t(rand() % 200 - 100);

    auto t_of = [&](auto fn, int reps) {
        fn();
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; r++) fn();
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
    };

    double gflop = 2.0 * S * K * N / 1e9;
    auto rep = [&](const char* name, double ms) {
        printf("%-16s %8.1f ms   %7.1f GFLOP/s\n", name, ms, gflop / (ms * 1e-3));
    };
    rep("i8 + scale", t_of([&] {
        gemm_i8_full(X.data(), S, K, Wi.data(), scale.data(), N, Y.data(),
                     scratch.data());
    }, 3));
    rep("i8 no scale", t_of([&] {
        gemm_i8_noscale(X.data(), S, K, Wi.data(), scale.data(), N, Y.data(),
                        scratch.data());
    }, 3));
    rep("f32 + scale", t_of([&] {
        gemm_f32_scale(X.data(), S, K, Wf.data(), scale.data(), N, Y.data(),
                       scratch.data());
    }, 3));
    return 0;
}
