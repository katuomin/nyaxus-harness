// Microbench: compare nyaxus_gemm_avx (f32 W) vs nyaxus_gemm_i8_avx (int8 W)
// on identical shapes, to isolate kernel cost from model load / noise.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int nyaxus_cpu_has_avx_fma();
extern "C" void nyaxus_gemm_avx(const float* X, int S, int K, const float* W,
                                int N, float* Y, float* scratch);
extern "C" void nyaxus_gemm_i8_avx(const float* X, int S, int K, const int8_t* W,
                                   const float* scale, int N, float* Y,
                                   float* scratch);

int main() {
    printf("avx_fma=%d\n", nyaxus_cpu_has_avx_fma());
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
        fn(); // warm
        auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < reps; r++) fn();
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
    };

    double ms_f = t_of([&] {
        nyaxus_gemm_avx(X.data(), S, K, Wf.data(), N, Y.data(), scratch.data());
    }, 3);
    double ms_i = t_of([&] {
        nyaxus_gemm_i8_avx(X.data(), S, K, Wi.data(), scale.data(), N, Y.data(),
                           scratch.data());
    }, 3);
    double gflop = 2.0 * S * K * N / 1e9;
    printf("f32 W: %8.1f ms   %7.1f GFLOP/s\n", ms_f, gflop / (ms_f * 1e-3));
    printf("i8  W: %8.1f ms   %7.1f GFLOP/s\n", ms_i, gflop / (ms_i * 1e-3));
    return 0;
}
