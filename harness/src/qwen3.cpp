#include "nyaxus/qwen3.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <span>
#include <thread>

namespace nyaxus {
namespace qwen3 {

namespace {

constexpr uint32_t kMagic = 0x3358594E;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kArchQwen3 = 1;

float bf16_to_f32(uint16_t b) {
    uint32_t u = uint32_t(b) << 16;
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Read a tensor's raw BF16 bytes and upcast to FP32.
std::vector<float> read_tensor_f32(const std::vector<char>& file, uint64_t offset,
                                   uint64_t nbytes) {
    size_t count = size_t(nbytes / 2);
    std::vector<float> out(count);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(file.data()) + offset;
    for (size_t i = 0; i < count; i++) {
        uint16_t b = uint16_t(p[2 * i]) | (uint16_t(p[2 * i + 1]) << 8);
        out[i] = bf16_to_f32(b);
    }
    return out;
}

// Read a tensor stored as raw FP32 bytes (INT8-mode per-row scales).
std::vector<float> read_tensor_raw_f32(const std::vector<char>& file,
                                       uint64_t offset, uint64_t nbytes) {
    std::vector<float> out(nbytes / 4);
    std::memcpy(out.data(), file.data() + offset, nbytes);
    return out;
}

// Read a tensor stored as raw INT8 bytes (one byte per element).
std::vector<int8_t> read_tensor_i8(const std::vector<char>& file,
                                   uint64_t offset, uint64_t nbytes) {
    std::vector<int8_t> out(nbytes);
    std::memcpy(out.data(), file.data() + offset, nbytes);
    return out;
}

// SIMD dispatch: 2 = AVX+FMA (256-bit), 1 = SSE2 (128-bit), 0 = scalar.
// The 256-bit path needs only AVX+FMA (not AVX2) - a pure FP32 matmul uses no
// AVX2-specific instructions - so it runs on any AVX+FMA CPU. Kernels live in
// translation units compiled with their own -m flags; scalar is the non-x86
// fallback.
#if defined(__x86_64__) || defined(_M_X64)
extern "C" int nyaxus_cpu_has_avx_fma();
extern "C" void nyaxus_gemm_sse2(const float* X, int S, int K, const float* W,
                                 int N, float* Y, float* scratch);
extern "C" void nyaxus_gemm_avx(const float* X, int S, int K, const float* W,
                                int N, float* Y, float* scratch);
extern "C" float nyaxus_dot_sse2(const float* a, const float* b, int n);
extern "C" float nyaxus_dot_avx(const float* a, const float* b, int n);
extern "C" void nyaxus_axpy_sse2(float* y, float a, const float* x, int n);
extern "C" void nyaxus_axpy_avx(float* y, float a, const float* x, int n);
extern "C" void nyaxus_softmax_sse2(const float* x, int n, float maxlogit,
                                    float* out);
extern "C" void nyaxus_softmax_avx(const float* x, int n, float maxlogit,
                                   float* out);
extern "C" void nyaxus_swiglu_sse2(const float* gate, const float* up, int n,
                                   float* out);
extern "C" void nyaxus_swiglu_avx(const float* gate, const float* up, int n,
                                  float* out);
extern "C" void nyaxus_gemm_i8_sse2(const float* X, int S, int K, const int8_t* W,
                                  const float* scale, int N, float* Y,
                                  float* scratch);
extern "C" void nyaxus_gemm_i8_avx(const float* X, int S, int K, const int8_t* W,
                                  const float* scale, int N, float* Y,
                                  float* scratch);
// Block-range variants: process only neuron blocks [b0, b1) (block = BN
// neurons). Used by the thread pool to split the GEMM over output neurons.
extern "C" void nyaxus_gemm_range_avx(const float* X, int S, int K,
                                      const float* W, int N, float* Y,
                                      float* scratch, int b0, int b1);
extern "C" void nyaxus_gemm_range_sse2(const float* X, int S, int K,
                                       const float* W, int N, float* Y,
                                       float* scratch, int b0, int b1);
extern "C" void nyaxus_gemm_i8_range_avx(const float* X, int S, int K,
                                         const int8_t* W, const float* scale,
                                         int N, float* Y, float* scratch,
                                         int b0, int b1);
extern "C" void nyaxus_gemm_i8_range_sse2(const float* X, int S, int K,
                                          const int8_t* W, const float* scale,
                                          int N, float* Y, float* scratch,
                                          int b0, int b1);
// AVX2 integer path: exact int16×int8 dot products (vpmaddwd).
// X is pre-quantized to int16 per row (quantize_rows_i16); no scratch needed.
extern "C" int nyaxus_cpu_has_avx2();
extern "C" void nyaxus_gemm_i16_i8_avx2(const int16_t* Xi16, const float* sx,
                                       int S, int K, const int8_t* W,
                                       const float* scale, int N, float* Y);
extern "C" void nyaxus_gemm_i16_i8_range_avx2(const int16_t* Xi16,
                                             const float* sx, int S, int K,
                                             const int8_t* W,
                                             const float* scale, int N,
                                             float* Y, int b0, int b1);

int simd_mode() {
    static const int mode = [] {
        try {
            return nyaxus_cpu_has_avx_fma() ? 2 : 1;
        } catch (...) {
            return 1;
        }
    }();
    return mode;
}
#endif

// ---- Persistent thread pool for N-split GEMM ----
//
// Spawning threads per GEMM would cost ~50-100 µs of startup per call, and a
// single forward pass issues ~196 GEMMs; a persistent pool reduces the
// wake-up cost to a few µs. The split is over OUTPUT NEURONS (N), not the K
// reduction, so every output element is computed by exactly one thread with
// exactly the arithmetic of the single-threaded kernel - results are
// bit-identical. std::jthread + condition variables only; no external deps.
class GemmPool {
public:
    explicit GemmPool(int n) {
        for (int i = 0; i < n; i++)
            workers_.emplace_back([this](std::stop_token st) { worker_loop(st); });
    }
    // No body: workers_ is declared last, so it is destroyed first. Each
    // jthread owns the stop source its worker listens to (the token the
    // jthread passes to the callable); its destructor requests that stop
    // (waking the worker from the stop-token cv wait) and joins it, all
    // before the dispatch state declared above (job_, t_, ...) is destroyed.
    // A woken worker may still be reading that state in its predicate, so
    // the join must precede its destruction.
    ~GemmPool() {}
    int size() const { return static_cast<int>(workers_.size()); }

    // Splits [0, n) into at most size() contiguous chunks; chunk i with
    // bounds [lo, hi) runs fn(i, lo, hi). Blocks until all chunks complete.
    //
    // Dispatch is serialized: job_/next_/active_ are single-slot state, so a
    // second concurrent dispatch would re-arm the completion counter and
    // reset the slot cursor mid-flight, stranding the first dispatcher's
    // wait (observed deadlock: a run's recall rerank overlapping a title
    // generation). Concurrent callers queue; they would contend for the same
    // cores anyway.
    void parallel_for(int n, const std::function<void(int, int, int)>& fn) {
        int t = std::min(size(), n);
        if (t <= 1) {
            fn(0, 0, n);
            return;
        }
        std::lock_guard<std::mutex> dispatch_lk(dispatch_mu_);
        std::unique_lock<std::mutex> lk(mu_);
        job_ = fn;
        n_ = n;
        t_ = t;
        next_ = 0;
        active_ = t;
        cv_.notify_all();
        // <= 0, not == 0: when t < size(), surplus workers (spurious or
        // no-slot wakeups) decrement past zero; if they land before the
        // dispatcher re-acquires the lock, an == 0 predicate would never
        // re-fire and the wait would hang.
        done_cv_.wait(lk, [this] { return active_ <= 0; });
        job_ = nullptr;
    }

private:
    void worker_loop(std::stop_token st) {
        for (;;) {
            std::function<void(int, int, int)> job;
            int idx = -1;
            {
                std::unique_lock<std::mutex> lk(mu_);
                // Wake only while there is a job AND an unclaimed slot, so a
                // worker never double-claims across dispatches. The stop-
                // token overload is race-free against a concurrent
                // request_stop (plain condition_variable lacks the overload
                // in libstdc++; only condition_variable_any has it).
                cv_.wait(lk, st, [this, st] {
                    return st.stop_requested() || (job_ && next_ < t_);
                });
                if (st.stop_requested()) return;
                idx = next_++;
                if (idx >= t_) {
                    active_--;
                    if (active_ == 0) done_cv_.notify_all();
                    continue;
                }
                job = job_;
            }
            int lo = n_ * idx / t_;
            int hi = n_ * (idx + 1) / t_;
            job(idx, lo, hi);
            {
                std::lock_guard<std::mutex> lk(mu_);
                active_--;
                if (active_ == 0) done_cv_.notify_all();
            }
        }
    }

    std::mutex dispatch_mu_;
    std::mutex mu_;
    // condition_variable_any: the only cv type with the C++20 stop-token
    // wait overloads in libstdc++; with unique_lock<mutex> it behaves the
    // same as condition_variable.
    std::condition_variable_any cv_;
    std::condition_variable done_cv_;
    std::function<void(int, int, int)> job_;
    int n_ = 0;
    int t_ = 0;
    int next_ = 0;
    int active_ = 0;
    // Declared last so it is destroyed first (see the destructor above).
    std::vector<std::jthread> workers_;
};

// Physical-core estimate: hardware_concurrency() reports LOGICAL cores (16
// on an 8-core SMT machine); SMT adds little to FMA-bound code and doubles
// L1/L2 pressure, so halve it. Override with NYAXUS_ENGINE_THREADS (1 = off).
int gemm_thread_count() {
    if (const char* e = std::getenv("NYAXUS_ENGINE_THREADS")) {
        int v = std::atoi(e);
        if (v >= 1) return v;
    }
    int phys = static_cast<int>(std::thread::hardware_concurrency()) / 2;
    if (phys < 1) phys = 1;
    if (phys > 32) phys = 32;
    return phys;
}

GemmPool& gemm_pool() {
    static GemmPool pool(gemm_thread_count());
    return pool;
}

// Below this token count, thread wake-up + barrier cost exceeds the parallel
// speedup; keep the single-threaded path.
constexpr int kMinThreadTokens = 8;

// Quantize X rows to int16 with a per-row scale, for the AVX2 integer GEMM:
//   sx[m]   = max_k |x[m][k]| / 5461   (0 if the row is all zeros)
//   x_i16[k] = round(x[m][k] / sx[m])
// The 5461 cap (instead of 32767) keeps the int32 accumulation exact for K up
// to 3072: |sum| <= 5461 * 127 * 3072 < 2^31. 12.4 bits of mantissa - the
// earlier 7-bit uint8 scheme lost ~3% per GEMM, which compounded across 28
// layers and washed out the reranker signal. The division (not a reciprocal
// multiply) is deliberate: the scalar reference uses the identical
// expression, keeping the two bit-identical.
void quantize_rows_i16(const float* X, int S, int K, std::vector<int16_t>& xi16,
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

// Persistent KV cache for rerank_many: grown once and reused across calls
// (a fresh cache per call was ~19-57 MB of allocation churn per recall).
// NOT thread-safe (same invariant as I16Scratch).
KVState& rerank_kv() {
    static KVState st;
    return st;
}

// Persistent scratch for the AVX2 int16×int8 path: the quantized X and the
// per-row scales. Grown as needed and reused across GEMMs, so the hot path
// does no per-call heap allocation.
// NOT thread-safe: two concurrent forward() calls would share these buffers.
// Serialized by engine_mu() (see below) - the public inference entry points
// all take it, so concurrent callers queue instead of racing on this state.
struct I16Scratch {
    std::vector<int16_t> xi16;
    std::vector<float> sx;
};
I16Scratch& i16_scratch() {
    static I16Scratch s;
    return s;
}

// Process-wide inference lock. forward(), gemm_w(), and the rerank_many KV
// cache share the non-thread-safe state above; every public inference entry
// point (embed_ids, rerank_ids, rerank_many, generate, forward) takes this
// exactly once. The internal *_impl helpers are lock-free and must only be
// called while holding it.
std::mutex& engine_mu() {
    static std::mutex mu;
    return mu;
}

// Y = X · Wᵀ,  X: [S,K] row-major, W: [N,K] row-major, Y: [S,N] row-major.
//
// The per-token matvec re-streamed every weight from RAM once per token
// (~1.5 GB/token for this model) - bandwidth-bound. The batched GEMM reads
// each weight once and applies it to all S tokens. For S >= kMinThreadTokens
// the neuron blocks are split across the persistent pool (bit-identical
// results; per-thread scratch avoids false sharing). `scratch` must hold
// (BK*BN + S*BN) floats for the single-threaded path. Spans are zero-cost
// views: the dimensions (S, K, N) still bound the kernels, so the arithmetic
// is unchanged and the results stay bit-identical.
void gemm(std::span<const float> X, int S, int K, std::span<const float> W,
          int N, std::span<float> Y, std::span<float> scratch) {
#if defined(__x86_64__) || defined(_M_X64)
    if (simd_mode() == 2) {
        if (S >= kMinThreadTokens) {
            int blocks = (N + 7) / 8;
            GemmPool& pool = gemm_pool();
            int t = std::min(pool.size(), blocks);
            if (t > 1) {
                // Per-thread scratch: Wt tile (BK*BN) + partial sums (S*BN),
                // BN=8, BK=64 per the AVX kernel.
                const std::size_t need = 64 * 8 + static_cast<std::size_t>(S) * 8;
                std::unique_ptr<float[]> thr(new float[static_cast<std::size_t>(t) * need]);
                pool.parallel_for(blocks, [&](int idx, int lo, int hi) {
                    nyaxus_gemm_range_avx(X.data(), S, K, W.data(), N, Y.data(),
                                          thr.get() + static_cast<std::size_t>(idx) * need,
                                          lo, hi);
                });
                return;
            }
        }
        nyaxus_gemm_avx(X.data(), S, K, W.data(), N, Y.data(), scratch.data());
    } else {
        if (S >= kMinThreadTokens) {
            int blocks = (N + 3) / 4;
            GemmPool& pool = gemm_pool();
            int t = std::min(pool.size(), blocks);
            if (t > 1) {
                // Per-thread scratch: Wt tile (BK*BN) + partial sums (S*BN),
                // BN=4, BK=64 per the SSE2 kernel.
                const std::size_t need = 64 * 4 + static_cast<std::size_t>(S) * 4;
                std::unique_ptr<float[]> thr(new float[static_cast<std::size_t>(t) * need]);
                pool.parallel_for(blocks, [&](int idx, int lo, int hi) {
                    nyaxus_gemm_range_sse2(X.data(), S, K, W.data(), N, Y.data(),
                                           thr.get() + static_cast<std::size_t>(idx) * need,
                                           lo, hi);
                });
                return;
            }
        }
        nyaxus_gemm_sse2(X.data(), S, K, W.data(), N, Y.data(), scratch.data());
    }
#else
    for (int i = 0; i < S; i++)
        for (int o = 0; o < N; o++) {
            float acc = 0.0f;
            const float* xrow = X.data() + size_t(i) * K;
            const float* wrow = W.data() + size_t(o) * K;
            for (int j = 0; j < K; j++) acc += xrow[j] * wrow[j];
            Y[size_t(i) * N + o] = acc;
        }
#endif
}

// INT8 variant: Y = (X · Wᵀ) ⊙ scale,  W: [N,K] int8 (original layout),
// scale: [N].
//
// Path selection (best first):
//   1. AVX2 integer GEMM (exact int16×int8 dot products; X quantized to
//      int16 once per call, shared by all threads; no scratch, ½ weight
//      traffic);
//   2. AVX-FP32 (int8 converted to FP32 per K chunk) - the fallback on CPUs
//      without AVX2;
//   3. SSE2-FP32; 4. scalar.
// The neuron blocks are split across the persistent pool when S is large
// enough (bit-identical results; per-thread scratch for the FP32 paths).
void gemm_i8(std::span<const float> X, int S, int K, std::span<const int8_t> W,
             std::span<const float> scale, int N, std::span<float> Y,
             std::span<float> scratch) {
#if defined(__x86_64__) || defined(_M_X64)
    if (simd_mode() == 2 && nyaxus_cpu_has_avx2()) {
        I16Scratch& sc = i16_scratch();
        quantize_rows_i16(X.data(), S, K, sc.xi16, sc.sx);
        int blocks = N;  // per-neuron split; the kernel groups 8 neurons per X load
        if (S >= kMinThreadTokens) {
            GemmPool& pool = gemm_pool();
            int t = std::min(pool.size(), blocks);
            if (t > 1) {
                pool.parallel_for(blocks, [&](int, int lo, int hi) {
                    nyaxus_gemm_i16_i8_range_avx2(sc.xi16.data(), sc.sx.data(), S, K,
                                                  W.data(), scale.data(), N, Y.data(),
                                                  lo, hi);
                });
                return;
            }
        }
        nyaxus_gemm_i16_i8_avx2(sc.xi16.data(), sc.sx.data(), S, K, W.data(),
                                scale.data(), N, Y.data());
        return;
    }
    if (simd_mode() == 2) {
        if (S >= kMinThreadTokens) {
            int blocks = (N + 7) / 8;
            GemmPool& pool = gemm_pool();
            int t = std::min(pool.size(), blocks);
            if (t > 1) {
                const std::size_t need = 64 * 8 + static_cast<std::size_t>(S) * 8;
                std::unique_ptr<float[]> thr(new float[static_cast<std::size_t>(t) * need]);
                pool.parallel_for(blocks, [&](int idx, int lo, int hi) {
                    nyaxus_gemm_i8_range_avx(X.data(), S, K, W.data(), scale.data(),
                                             N, Y.data(),
                                             thr.get() + static_cast<std::size_t>(idx) * need,
                                             lo, hi);
                });
                return;
            }
        }
        nyaxus_gemm_i8_avx(X.data(), S, K, W.data(), scale.data(), N, Y.data(),
                           scratch.data());
    } else {
        if (S >= kMinThreadTokens) {
            int blocks = (N + 3) / 4;
            GemmPool& pool = gemm_pool();
            int t = std::min(pool.size(), blocks);
            if (t > 1) {
                const std::size_t need = 64 * 4 + static_cast<std::size_t>(S) * 4;
                std::unique_ptr<float[]> thr(new float[static_cast<std::size_t>(t) * need]);
                pool.parallel_for(blocks, [&](int idx, int lo, int hi) {
                    nyaxus_gemm_i8_range_sse2(X.data(), S, K, W.data(), scale.data(),
                                              N, Y.data(),
                                              thr.get() + static_cast<std::size_t>(idx) * need,
                                              lo, hi);
                });
                return;
            }
        }
        nyaxus_gemm_i8_sse2(X.data(), S, K, W.data(), scale.data(), N, Y.data(),
                            scratch.data());
    }
#else
    for (int i = 0; i < S; i++)
        for (int o = 0; o < N; o++) {
            float acc = 0.0f;
            const float* xrow = X.data() + size_t(i) * K;
            const int8_t* wrow = W.data() + size_t(o) * K;
            for (int j = 0; j < K; j++) acc += xrow[j] * float(wrow[j]);
            Y[size_t(i) * N + o] = acc * scale[o];
        }
#endif
}

// Dispatch over the Weight storage mode (BF16 vs INT8).
void gemm_w(std::span<const float> X, int S, int K, const Weight& W, int N,
            std::span<float> Y, std::span<float> scratch) {
    if (W.i8.empty()) {
        gemm(X, S, K, W.f32, N, Y, scratch);
        return;
    }
    gemm_i8(X, S, K, W.i8, W.scale, N, Y, scratch);
}

float dotv(const float* a, const float* b, int n) {
#if defined(__x86_64__) || defined(_M_X64)
    if (simd_mode() == 2) return nyaxus_dot_avx(a, b, n);
    return nyaxus_dot_sse2(a, b, n);
#else
    float t = 0.0f;
    for (int j = 0; j < n; j++) t += a[j] * b[j];
    return t;
#endif
}

void axpy(float* y, float a, const float* x, int n) {
#if defined(__x86_64__) || defined(_M_X64)
    if (simd_mode() == 2) nyaxus_axpy_avx(y, a, x, n);
    else nyaxus_axpy_sse2(y, a, x, n);
#else
    for (int j = 0; j < n; j++) y[j] += a * x[j];
#endif
}

// Softmax of x[0..n) given the precomputed max: exp(x - max) / sum. SIMD
// fast-exp inside; a degree-5 2^f poly keeps the error ~3e-6 relative.
// Below 8 elements (one vector iteration) the SIMD setup doesn't amortize,
// so use plain exp; from 8 up the vector body beats scalar exp even with a
// scalar tail.
void softmax(const float* x, int n, float maxlogit, float* out) {
#if defined(__x86_64__) || defined(_M_X64)
    if (n >= 8) {
        if (simd_mode() == 2) nyaxus_softmax_avx(x, n, maxlogit, out);
        else nyaxus_softmax_sse2(x, n, maxlogit, out);
        return;
    }
#endif
    float sum = 0.0f;
    for (int i = 0; i < n; i++) {
        out[i] = std::exp(x[i] - maxlogit);
        sum += out[i];
    }
    for (int i = 0; i < n; i++) out[i] /= sum;
}

// SiLU gate: out[j] = gate[j] * sigmoid(gate[j]) * up[j].
void swiglu(const float* gate, const float* up, int n, float* out) {
#if defined(__x86_64__) || defined(_M_X64)
    if (simd_mode() == 2) nyaxus_swiglu_avx(gate, up, n, out);
    else nyaxus_swiglu_sse2(gate, up, n, out);
#else
    for (int j = 0; j < n; j++) {
        float g = gate[j];
        float sig = 1.0f / (1.0f + std::exp(-g));
        out[j] = g * sig * up[j];
    }
#endif
}

void rmsnorm(const float* x, int n, const std::vector<float>& weight, float eps,
             float* y) {
    float ss = 0.0f;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float inv = 1.0f / std::sqrt(ss / n + eps);
    for (int i = 0; i < n; i++) y[i] = x[i] * inv * weight[i];
}


// ---- Tokenizer helpers (verified against the Qwen3 byte-fallback table) ----

// byte -> codepoint
int byte_to_cp(uint8_t b) {
    if (b <= 32) return 256 + b;
    if (b >= 127 && b <= 160) return b + 162;
    if (b == 173) return 323;
    return b;  // 33..126, 161..172, 174..255
}

// codepoint -> byte
int cp_to_byte(int cp) {
    if (cp >= 256 && cp <= 288) return cp - 256;
    if (cp >= 289 && cp <= 322) return cp - 162;
    if (cp == 323) return 173;
    return cp;  // 33..126, 161..172, 174..255
}

bool is_ascii_letter(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
bool is_ascii_digit(uint8_t c) { return c >= '0' && c <= '9'; }
bool is_space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' ||
           c == '\f';
}

bool is_contraction(const std::string& s) {
    // Case-insensitive match of 's 't 're 've 'm 'll 'd
    auto lower = s;
    for (auto& c : lower) c = char(tolower(uint8_t(c)));
    static const std::vector<std::string> k = {"'s", "'t", "'re", "'ve", "'m",
                                               "'ll", "'d"};
    for (auto& t : k)
        if (lower == t) return true;
    return false;
}

// Split text into pre-tokens approximating the Qwen pre-tokenizer for ASCII.
std::vector<std::string> pre_tokenize(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    size_t n = text.size();
    while (i < n) {
        uint8_t c = uint8_t(text[i]);
        // 1. Contraction
        if (c == '\'') {
            std::string cand;
            size_t j = i;
            cand += text[j++];
            while (j < n && is_ascii_letter(uint8_t(text[j])) && cand.size() <= 3)
                cand += text[j++];
            if (is_contraction(cand)) {
                out.push_back(cand);
                i = j;
                continue;
            }
        }
        // 2. Optional leading punct + letters
        if (is_ascii_letter(c)) {
            size_t j = i;
            while (j < n && is_ascii_letter(uint8_t(text[j]))) j++;
            out.push_back(text.substr(i, j - i));
            i = j;
            continue;
        }
        if (c != '\r' && c != '\n' && !is_ascii_letter(c) && !is_ascii_digit(c) &&
            i + 1 < n && is_ascii_letter(uint8_t(text[i + 1]))) {
            size_t j = i + 1;
            while (j < n && is_ascii_letter(uint8_t(text[j]))) j++;
            out.push_back(text.substr(i, j - i));
            i = j;
            continue;
        }
        // 3. Single digit
        if (is_ascii_digit(c)) {
            out.push_back(std::string(1, c));
            i++;
            continue;
        }
        // 5-7. Whitespace run
        if (is_space(c)) {
            size_t j = i;
            while (j < n && is_space(uint8_t(text[j]))) j++;
            out.push_back(text.substr(i, j - i));
            i = j;
            continue;
        }
        // 4. Symbol run (non-space, non-letter, non-digit) + trailing newlines
        {
            size_t j = i;
            while (j < n && !is_space(uint8_t(text[j])) &&
                   !is_ascii_letter(uint8_t(text[j])) &&
                   !is_ascii_digit(uint8_t(text[j])))
                j++;
            while (j < n && (text[j] == '\r' || text[j] == '\n')) j++;
            if (j > i) {
                out.push_back(text.substr(i, j - i));
                i = j;
                continue;
            }
        }
        // Fallback
        out.push_back(std::string(1, c));
        i++;
    }
    return out;
}

// Encode one pre-token (a UTF-8 string) into BPE token strings.
std::vector<std::string> bpe_encode(const Model& m, const std::string& word) {
    // Build the initial sequence of single-codepoint tokens.
    std::vector<std::string> tokens;
    for (size_t i = 0; i < word.size();) {
        // Decode UTF-8 codepoint.
        uint8_t b0 = uint8_t(word[i]);
        int cp;
        int len;
        if (b0 < 0x80) { cp = b0; len = 1; }
        else if ((b0 >> 5) == 0x6) { cp = b0 & 0x1F; len = 2; }
        else if ((b0 >> 4) == 0xE) { cp = b0 & 0x0F; len = 3; }
        else { cp = b0 & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < word.size(); k++)
            cp = (cp << 6) | (uint8_t(word[i + k]) & 0x3F);
        i += len;
        // Map to the byte-fallback codepoint.
        int fallback_cp = byte_to_cp(uint8_t(b0));
        (void)cp;
        // Represent as a single UTF-8 char of fallback_cp.
        std::string t;
        if (fallback_cp < 0x80) t += char(fallback_cp);
        else if (fallback_cp < 0x800) {
            t += char(0xC0 | (fallback_cp >> 6));
            t += char(0x80 | (fallback_cp & 0x3F));
        } else {
            t += char(0xE0 | (fallback_cp >> 12));
            t += char(0x80 | ((fallback_cp >> 6) & 0x3F));
            t += char(0x80 | (fallback_cp & 0x3F));
        }
        tokens.push_back(t);
    }

    // Iteratively apply the lowest-rank merge (rank map built once in
    // load(), resident on the model).
    const std::map<std::pair<std::string, std::string>, int>& rank =
        m.merge_rank;
    bool changed = true;
    while (changed) {
        changed = false;
        int best = -1;
        int best_rank = INT32_MAX;
        for (size_t i = 0; i + 1 < tokens.size(); i++) {
            auto it = rank.find(std::make_pair(tokens[i], tokens[i + 1]));
            if (it != rank.end() && it->second < best_rank) {
                best_rank = it->second;
                best = int(i);
            }
        }
        if (best >= 0) {
            std::string merged = tokens[best] + tokens[best + 1];
            tokens.erase(tokens.begin() + best, tokens.begin() + best + 2);
            tokens.insert(tokens.begin() + best, merged);
            changed = true;
        }
    }
    return tokens;
}

}  // namespace

// Number of worker threads the engine will use for parallel GEMM (1 = the
// single-threaded path). Exposed for diagnostics (bench header, daemon log).
int engine_thread_count() { return gemm_thread_count(); }

Model load(const std::string& path, std::string& error) {
    Model m;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return m;
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
    if (buf.size() < 88) {
        error = "file too small";
        return m;
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(buf.data());
    auto u32 = [&](size_t off) {
        return uint32_t(p[off]) | (uint32_t(p[off + 1]) << 8) |
               (uint32_t(p[off + 2]) << 16) | (uint32_t(p[off + 3]) << 24);
    };
    auto f32 = [&](size_t off) {
        uint32_t u = u32(off);
        float x;
        std::memcpy(&x, &u, 4);
        return x;
    };
    if (u32(0) != kMagic) { error = "bad magic"; return m; }
    if (u32(4) != kVersion) { error = "bad version"; return m; }
    if (u32(8) != kArchQwen3) { error = "bad arch"; return m; }

    m.task = u32(12);
    m.hidden = u32(16);
    m.layers = u32(20);
    m.heads = u32(24);
    m.kv_heads = u32(28);
    m.head_dim = u32(32);
    m.inter = u32(36);
    m.tokenizer_size = u32(40);
    m.vocab_rows = u32(44);
    m.max_pos = u32(48);
    m.eos = u32(52);
    m.quant = u32(56);
    if (m.quant != 0 && m.quant != 1) {
        error = "unsupported quant mode: " + std::to_string(m.quant);
        return m;
    }
    m.true_id = u32(60);
    m.false_id = u32(64);
    uint32_t num_tensors = u32(68);
    uint32_t vocab_entries = u32(72);
    uint32_t num_merges = u32(76);
    m.rope_theta = f32(80);
    m.rms_eps = f32(84);

    // Sanity: each index entry is at least 8 bytes (vocab, merges) or 4
    // (tensor index), so inflated counts from a corrupt header are caught
    // before any out-of-bounds read.
    if (vocab_entries > buf.size() / 8 || num_merges > buf.size() / 8 ||
        num_tensors > buf.size() / 4) {
        error = "corrupt header counts";
        return m;
    }

    size_t off = 88;
    bool bad = false;  // the index ran past the end of the buffer
    auto u32b = [&](size_t at) {
        if (at + 4 > buf.size()) { bad = true; return uint32_t(0); }
        return u32(at);
    };
    // Vocab
    m.id2tok.resize(m.tokenizer_size);
    for (uint32_t i = 0; i < vocab_entries; i++) {
        uint32_t id = u32b(off);
        off += 4;
        uint32_t len = u32b(off);
        off += 4;
        if (off + len > buf.size()) { bad = true; len = 0; }
        std::string s(buf.begin() + off, buf.begin() + off + len);
        off += len;
        if (bad) break;
        if (id < m.id2tok.size()) {
            m.id2tok[id] = s;
            m.tok2id[s] = int(id);
        }
    }
    if (bad) { error = "truncated vocab index"; return m; }
    // Merges
    m.merges.resize(num_merges);
    for (uint32_t i = 0; i < num_merges; i++) {
        m.merges[i].first = int(u32b(off));
        off += 4;
        m.merges[i].second = int(u32b(off));
        off += 4;
    }
    if (bad) { error = "truncated merges index"; return m; }
    // (left,right) -> rank map for BPE, built once per model.
    for (std::size_t r = 0; r < m.merges.size(); r++) {
        int l = m.merges[r].first;
        int rr = m.merges[r].second;
        if (l < (int)m.id2tok.size() && rr < (int)m.id2tok.size())
            m.merge_rank[std::make_pair(m.id2tok[l], m.id2tok[rr])] = int(r);
    }
    // Tensor index
    struct TEntry {
        std::string name;
        std::vector<int> dims;
        uint64_t data_offset;
        uint64_t data_len;
    };
    std::vector<TEntry> entries;
    for (uint32_t i = 0; i < num_tensors; i++) {
        TEntry e;
        uint32_t nlen = u32b(off);
        off += 4;
        if (off + nlen > buf.size()) { bad = true; nlen = 0; }
        e.name.assign(buf.begin() + off, buf.begin() + off + nlen);
        off += nlen;
        uint32_t ndim = u32b(off);
        off += 4;
        if (ndim > 8) { bad = true; ndim = 0; }  // no tensor here has >2 dims
        e.dims.resize(ndim);
        for (uint32_t d = 0; d < ndim; d++) {
            e.dims[d] = int(u32b(off));
            off += 4;
        }
        e.data_offset = uint64_t(u32b(off)) | (uint64_t(u32b(off + 4)) << 32);
        off += 8;
        e.data_len = uint64_t(u32b(off)) | (uint64_t(u32b(off + 4)) << 32);
        off += 8;
        if (bad) break;
        entries.push_back(std::move(e));
    }
    if (bad) { error = "truncated tensor index"; return m; }
    // Route tensors. Destinations start empty and are filled by move; the
    // exporter already validated shapes, so no pre-sizing or size check here.
    // In INT8 mode (quant=1) the seven linear projections are stored as raw
    // INT8 plus a per-row FP32 scale tensor named `<name>.scale`; everything
    // else stays BF16.
    m.layer.resize(m.layers);
    for (auto& e : entries) {
        if (e.data_offset + e.data_len > buf.size()) {
            error = "tensor " + e.name + " out of bounds";
            m.ok = false;
            return m;
        }
        const bool is_scale =
            e.name.size() >= 6 &&
            e.name.compare(e.name.size() - 6, 6, ".scale") == 0;
        const std::string base =
            is_scale ? e.name.substr(0, e.name.size() - 6) : e.name;
        auto read_bf16 = [&] {
            return read_tensor_f32(buf, e.data_offset, e.data_len);
        };
        auto assign = [&](std::vector<float>& dst) { dst = read_bf16(); };
        auto assign_proj = [&](Weight& w) {
            if (is_scale) w.scale = read_tensor_raw_f32(buf, e.data_offset,
                                                        e.data_len);
            else if (m.quant == 1)
                w.i8 = read_tensor_i8(buf, e.data_offset, e.data_len);
            else
                w.f32 = read_bf16();
        };
        if (base == "embed_tokens.weight") {
            if (is_scale) {
                error = "unexpected scale tensor " + e.name;
                m.ok = false;
                return m;
            }
            assign(m.embed);
        } else if (base == "norm.weight") {
            if (is_scale) {
                error = "unexpected scale tensor " + e.name;
                m.ok = false;
                return m;
            }
            assign(m.final_norm);
        } else if (base == "lm_head.weight") {
            if (is_scale) {
                error = "unexpected scale tensor " + e.name;
                m.ok = false;
                return m;
            }
            assign(m.lm_head);
        } else {
            // layers.N.<part>.weight
            // Parse N and part.
            size_t lp = e.name.find("layers.");
            if (lp != std::string::npos) {
                size_t np = lp + 7;
                int layer = 0;
                while (e.name[np] >= '0' && e.name[np] <= '9') {
                    layer = layer * 10 + (e.name[np] - '0');
                    np++;
                }
                if (layer < 0 || layer >= m.layers) {
                    error = "bad layer index in " + e.name;
                    m.ok = false;
                    return m;
                }
                Layer& L = m.layer[layer];
                const std::string rest = is_scale
                                             ? base.substr(np)
                                             : e.name.substr(np);
                if (rest == ".input_layernorm.weight") assign(L.input_ln);
                else if (rest == ".self_attn.q_proj.weight") assign_proj(L.q_proj);
                else if (rest == ".self_attn.k_proj.weight") assign_proj(L.k_proj);
                else if (rest == ".self_attn.v_proj.weight") assign_proj(L.v_proj);
                else if (rest == ".self_attn.o_proj.weight") assign_proj(L.o_proj);
                else if (rest == ".self_attn.q_norm.weight") assign(L.q_norm);
                else if (rest == ".self_attn.k_norm.weight") assign(L.k_norm);
                else if (rest == ".post_attention_layernorm.weight")
                    assign(L.post_ln);
                else if (rest == ".mlp.gate_proj.weight") assign_proj(L.gate_proj);
                else if (rest == ".mlp.up_proj.weight") assign_proj(L.up_proj);
                else if (rest == ".mlp.down_proj.weight") assign_proj(L.down_proj);
                else {
                    error = "unrouted tensor " + e.name;
                    m.ok = false;
                    return m;
                }
            } else {
                error = "unrouted tensor " + e.name;
                m.ok = false;
                return m;
            }
        }
    }
    if (!error.empty()) {
        m.ok = false;
        return m;
    }
    // All projections stay in the [N, K] (safetensors) layout - the FP32
    // kernels transpose a [BK, BN] tile per K chunk (a [K, N] layout aliases
    // in L1 at N=1024, see the kernel headers), and the AVX2 integer kernel
    // reads 16 k-values of ONE neuron contiguously: W[n*K + k..k+15].
    // No post-load processing is needed.
    m.ok = true;
    return m;
}

std::vector<int> encode(const Model& m, const std::string& text) {
    std::vector<int> ids;
    auto pre = pre_tokenize(text);
    for (auto& w : pre) {
        auto toks = bpe_encode(m, w);
        for (auto& t : toks) {
            auto it = m.tok2id.find(t);
            if (it != m.tok2id.end())
                ids.push_back(it->second);
            else
                ids.push_back(0);  // unknown (should not happen for valid text)
        }
    }
    return ids;
}

std::string decode(const Model& m, const std::vector<int>& ids) {
    std::string out;
    for (int id : ids) {
        if (id < 0 || id >= (int)m.id2tok.size()) continue;
        const std::string& t = m.id2tok[id];
        // Convert codepoints back to bytes.
        for (size_t i = 0; i < t.size();) {
            uint8_t b0 = uint8_t(t[i]);
            int cp;
            int len;
            if (b0 < 0x80) { cp = b0; len = 1; }
            else if ((b0 >> 5) == 0x6) { cp = b0 & 0x1F; len = 2; }
            else if ((b0 >> 4) == 0xE) { cp = b0 & 0x0F; len = 3; }
            else { cp = b0 & 0x07; len = 4; }
            for (int k = 1; k < len && i + k < t.size(); k++)
                cp = (cp << 6) | (uint8_t(t[i + k]) & 0x3F);
            i += len;
            int b = cp_to_byte(cp);
            out += char(b);
        }
    }
    return out;
}

// Lock-free core; the public forward() below serializes via engine_mu().
static std::vector<float> forward_impl(const Model& m,
                                       const std::vector<int>& ids,
                                       KVState* state) {
    int S = int(ids.size());
    int H = m.hidden;
    int QD = m.heads * m.head_dim;
    int KVD = m.kv_heads * m.head_dim;
    int group = m.heads / m.kv_heads;
    int half = m.head_dim / 2;
    int cached = state ? state->pos : 0;

    std::vector<float> h(size_t(S) * H);
    for (int s = 0; s < S; s++) {
        int id = ids[s];
        const float* e = m.embed.data() + size_t(id) * H;
        for (int j = 0; j < H; j++) h[size_t(s) * H + j] = e[j];
    }

    // RoPE cos/sin table: depends only on (position, pair index), so compute
    // it once per forward pass instead of per head/token/layer (the per-call
    // pow + cos + sin was a measurable cost). The angle uses the ABSOLUTE
    // position (cached + s) so decode steps continue the prompt's rotation
    // schedule, but the table is stored by LOCAL index s (the consumer reads
    // rope_cos[s * half + i]); indexing the table by absolute pos would run
    // past its S-row allocation on cached passes.
    std::vector<float> freq(half);  // 1/pow(theta, 2i/d) depends only on i
    for (int i = 0; i < half; i++)
        freq[i] = 1.0f / std::pow(m.rope_theta, (2.0f * i) / m.head_dim);
    std::vector<float> rope_cos(size_t(S) * half), rope_sin(size_t(S) * half);
    for (int s = 0; s < S; s++) {
        int pos = cached + s;
        for (int i = 0; i < half; i++) {
            float angle = float(pos) * freq[i];
            rope_cos[size_t(s) * half + i] = std::cos(angle);
            rope_sin[size_t(s) * half + i] = std::sin(angle);
        }
    }

    std::vector<float> h_norm(size_t(S) * H);
    std::vector<float> q_all(size_t(S) * QD);
    std::vector<float> k_all(size_t(S) * KVD);
    std::vector<float> v_all(size_t(S) * KVD);
    std::vector<float> attn_out(size_t(S) * QD);
    std::vector<float> mlp_gate(size_t(S) * m.inter);
    std::vector<float> mlp_up(size_t(S) * m.inter);
    std::vector<float> mlp_mid(size_t(S) * m.inter);
    std::vector<float> mlp_out(size_t(S) * H);
    std::vector<float> o_proj_out(size_t(S) * H);
    // Scratch for the single-threaded GEMM path: transposed W tile (BK*BN)
    // + partial sums (S*BN); the 256-bit kernel uses BN=8, BK=64 (the 128-bit
    // kernel needs less). The threaded path allocates its own per-thread
    // scratch inside gemm().
    std::vector<float> gemm_scratch(64 * 8 + static_cast<std::size_t>(S) * 8);
    std::vector<float> scores(cached + S);

    // Debug: dump the residual stream after every layer (gated by env var,
    // zero cost when unset). Layout: [layer][s][j]. Used to locate the first
    // diverging layer when comparing cached vs one-pass forwards.
    const char* dbg_env = std::getenv("NYAXUS_DEBUG_LAYERS");
    bool dbg = dbg_env && *dbg_env;
    std::vector<float> dbg_dump;
    if (dbg) dbg_dump.reserve(static_cast<std::size_t>(m.layers) * S * H);

    // Grow the KV cache capacity once, before the layer loop (the check is
    // layer-invariant; doing it per layer was a footgun).
    if (state && state->cap < cached + S) {
        int newcap = std::max(cached + S, std::max(state->cap * 2, 256));
        for (auto& kc : state->k) kc.resize(size_t(newcap) * KVD);
        for (auto& vc : state->v) vc.resize(size_t(newcap) * KVD);
        state->cap = newcap;
    }

    for (int l = 0; l < m.layers; l++) {
        const Layer& L = m.layer[l];
        // --- Attention projections (batched GEMM) ---
        for (int s = 0; s < S; s++)
            rmsnorm(h.data() + size_t(s) * H, H, L.input_ln, m.rms_eps,
                    h_norm.data() + size_t(s) * H);
        gemm_w(h_norm, S, H, L.q_proj, QD, q_all, gemm_scratch);
        gemm_w(h_norm, S, H, L.k_proj, KVD, k_all, gemm_scratch);
        gemm_w(h_norm, S, H, L.v_proj, KVD, v_all, gemm_scratch);
        // QK norm (per head) + RoPE (from the precomputed table)
        for (int s = 0; s < S; s++) {
            for (int hq = 0; hq < m.heads; hq++)
                rmsnorm(q_all.data() + size_t(s) * QD + hq * m.head_dim,
                        m.head_dim, L.q_norm, m.rms_eps,
                        q_all.data() + size_t(s) * QD + hq * m.head_dim);
            for (int hk = 0; hk < m.kv_heads; hk++)
                rmsnorm(k_all.data() + size_t(s) * KVD + hk * m.head_dim,
                        m.head_dim, L.k_norm, m.rms_eps,
                        k_all.data() + size_t(s) * KVD + hk * m.head_dim);
            const float* ctab = rope_cos.data() + size_t(s) * half;
            const float* stab = rope_sin.data() + size_t(s) * half;
            for (int hq = 0; hq < m.heads; hq++) {
                float* v = q_all.data() + size_t(s) * QD + hq * m.head_dim;
                for (int i = 0; i < half; i++) {
                    float x0 = v[i], x1 = v[i + half];
                    v[i] = x0 * ctab[i] - x1 * stab[i];
                    v[i + half] = x1 * ctab[i] + x0 * stab[i];
                }
            }
            for (int hk = 0; hk < m.kv_heads; hk++) {
                float* v = k_all.data() + size_t(s) * KVD + hk * m.head_dim;
                for (int i = 0; i < half; i++) {
                    float x0 = v[i], x1 = v[i + half];
                    v[i] = x0 * ctab[i] - x1 * stab[i];
                    v[i + half] = x1 * ctab[i] + x0 * stab[i];
                }
            }
        }
        // Append this layer's K/V to the cache (K is already RoPE-applied;
        // V is raw). Done before attention so the current tokens' K/V are
        // available to themselves.
        if (state) {
            std::memcpy(state->k[l].data() + size_t(cached) * KVD, k_all.data(),
                        size_t(S) * KVD * sizeof(float));
            std::memcpy(state->v[l].data() + size_t(cached) * KVD, v_all.data(),
                        size_t(S) * KVD * sizeof(float));
        }
        // Attention (causal, GQA) - dot/axpy are SIMD-dispatched. Token s
        // (absolute position cached+s) attends over t <= cached+s: the
        // cached positions first, then the current pass's positions.
        float scale = 1.0f / std::sqrt(float(m.head_dim));
        for (int s = 0; s < S; s++) {
            int abs_s = cached + s;
            for (int hq = 0; hq < m.heads; hq++) {
                int kv = hq / group;
                const float* q = q_all.data() + size_t(s) * QD + hq * m.head_dim;
                float maxlogit = -1e30f;
                int t = 0;
                if (state) {
                    const float* kc = state->k[l].data() + kv * m.head_dim;
                    for (; t < cached; t++) {
                        scores[t] = dotv(q, kc + size_t(t) * KVD, m.head_dim) * scale;
                        if (scores[t] > maxlogit) maxlogit = scores[t];
                    }
                }
                for (; t <= abs_s; t++) {
                    const float* k =
                        k_all.data() + size_t(t - cached) * KVD + kv * m.head_dim;
                    scores[t] = dotv(q, k, m.head_dim) * scale;
                    if (scores[t] > maxlogit) maxlogit = scores[t];
                }
                softmax(scores.data(), abs_s + 1, maxlogit, scores.data());
                float* o = attn_out.data() + size_t(s) * QD + hq * m.head_dim;
                for (int d = 0; d < m.head_dim; d++) o[d] = 0.0f;
                t = 0;
                if (state) {
                    const float* vc = state->v[l].data() + kv * m.head_dim;
                    for (; t < cached; t++)
                        axpy(o, scores[t], vc + size_t(t) * KVD, m.head_dim);
                }
                for (; t <= abs_s; t++)
                    axpy(o, scores[t], v_all.data() + size_t(t - cached) * KVD +
                                       kv * m.head_dim,
                         m.head_dim);
            }
        }
        // o_proj + residual (batched GEMM)
        gemm_w(attn_out, S, QD, L.o_proj, H, o_proj_out, gemm_scratch);
        for (int s = 0; s < S; s++)
            for (int j = 0; j < H; j++)
                h[size_t(s) * H + j] += o_proj_out[size_t(s) * H + j];
        // --- MLP (batched GEMM + SwiGLU) ---
        for (int s = 0; s < S; s++)
            rmsnorm(h.data() + size_t(s) * H, H, L.post_ln, m.rms_eps,
                    h_norm.data() + size_t(s) * H);
        gemm_w(h_norm, S, H, L.gate_proj, m.inter, mlp_gate, gemm_scratch);
        gemm_w(h_norm, S, H, L.up_proj, m.inter, mlp_up, gemm_scratch);
        // SiLU on the gate projection, multiplied by the up projection.
        for (int s = 0; s < S; s++)
            swiglu(mlp_gate.data() + size_t(s) * m.inter,
                   mlp_up.data() + size_t(s) * m.inter, m.inter,
                   mlp_mid.data() + size_t(s) * m.inter);
        gemm_w(mlp_mid, S, m.inter, L.down_proj, H, mlp_out, gemm_scratch);
        for (int s = 0; s < S; s++)
            for (int j = 0; j < H; j++)
                h[size_t(s) * H + j] += mlp_out[size_t(s) * H + j];
        if (dbg) dbg_dump.insert(dbg_dump.end(), h.begin(), h.end());
    }
    if (state) state->pos += S;
    if (dbg) {
        FILE* f = std::fopen(dbg_env, "wb");
        if (f) {
            std::fwrite(dbg_dump.data(), sizeof(float), dbg_dump.size(), f);
            std::fclose(f);
        }
    }
    // Final norm
    std::vector<float> out(size_t(S) * H);
    for (int s = 0; s < S; s++)
        rmsnorm(h.data() + size_t(s) * H, H, m.final_norm, m.rms_eps,
                out.data() + size_t(s) * H);
    return out;
}

std::vector<float> forward(const Model& m, const std::vector<int>& ids,
                           KVState* state) {
    std::lock_guard<std::mutex> lk(engine_mu());
    return forward_impl(m, ids, state);
}

std::vector<float> embed_ids(const Model& m, const std::vector<int>& ids) {
    std::lock_guard<std::mutex> lk(engine_mu());
    std::vector<float> h = forward_impl(m, ids, nullptr);
    int H = m.hidden;
    int S = int(ids.size());
    std::vector<float> v(H);
    const float* last = h.data() + size_t(S - 1) * H;
    float ss = 0.0f;
    for (int j = 0; j < H; j++) ss += last[j] * last[j];
    float inv = 1.0f / std::sqrt(ss + 1e-12f);
    for (int j = 0; j < H; j++) v[j] = last[j] * inv;
    return v;
}

std::vector<float> embed(const Model& m, const std::string& text) {
    auto ids = encode(m, text);
    // The embedding model pools the final position; the reference service
    // appends EOS so that position aggregates the whole sequence. Match it.
    ids.push_back(m.eos);
    return embed_ids(m, ids);
}

// Lock-free core; the public entry points serialize via engine_mu().
static float rerank_score_ids(const Model& m, const std::vector<int>& query_ids,
                              const std::vector<int>& doc_ids) {
    // Concatenate query + document (the reranker template is applied by the
    // caller in production; here we score the given id sequence directly).
    std::vector<int> seq = query_ids;
    seq.insert(seq.end(), doc_ids.begin(), doc_ids.end());
    std::vector<float> h = forward_impl(m, seq, nullptr);
    int H = m.hidden;
    int S = int(seq.size());
    const float* last = h.data() + size_t(S - 1) * H;
    float logit_true = 0.0f;
    float logit_false = 0.0f;
    const float* et = m.embed.data() + size_t(m.true_id) * H;
    const float* ef = m.embed.data() + size_t(m.false_id) * H;
    for (int j = 0; j < H; j++) {
        logit_true += last[j] * et[j];
        logit_false += last[j] * ef[j];
    }
    return logit_true - logit_false;
}

float rerank_ids(const Model& m, const std::vector<int>& query_ids,
                 const std::vector<int>& doc_ids) {
    std::lock_guard<std::mutex> lk(engine_mu());
    return rerank_score_ids(m, query_ids, doc_ids);
}

// The reranker chat template (~60 tokens of overhead over query+doc),
// split at the <Document>: boundary so a query's prefix can be encoded
// once and its KV cache reused across docs (rerank_many below). The
// prefix ends at a pre-token chunk boundary (the colon is a lone symbol
// run) and BPE merges never cross chunks, so encode(prefix) ++
// encode(suffix) == encode(full prompt) exactly. The space AFTER the
// colon must stay in the suffix: pre_tokenize merges "space+letter" into
// one chunk (the doc's first word), so giving the space to the prefix
// would split it out of that merge and shift the tokens.
static std::string build_rerank_prefix(const std::string& query) {
    std::string instruct =
        "Given a web search query, retrieve relevant passages that answer the "
        "query";
    return "<|im_start|>system\nJudge whether the Document meets the requirements "
           "based on the Query and the Instruct provided. Note that the answer can "
           "only be \"yes\" or \"no\".<|im_end|>\n"
           "<|im_start|>user\n<Instruct>: " +
           instruct + "\n<Query>: " + query + "\n<Document>:";
}

static std::string build_rerank_suffix(const std::string& doc) {
    return " " + doc + "<|im_end|>\n" + "<|im_start|>assistant\n";
}

static std::string build_rerank_prompt(const std::string& query,
                                       const std::string& doc) {
    return build_rerank_prefix(query) + build_rerank_suffix(doc);
}

static float rerank_one(const Model& m, const std::string& query,
                        const std::string& doc) {
    auto ids = encode(m, build_rerank_prompt(query, doc));
    return rerank_score_ids(m, ids, std::vector<int>{});
}

float rerank(const Model& m, const std::string& query, const std::string& doc) {
    std::lock_guard<std::mutex> lk(engine_mu());
    return rerank_one(m, query, doc);
}

// Token count of the full rerank prompt (template + query + doc) - the
// actual forward length, which is larger than query+doc alone.
int rerank_prompt_tokens(const Model& m, const std::string& query,
                         const std::string& doc) {
    return int(encode(m, build_rerank_prompt(query, doc)).size());
}

// Rerank several docs against one query, sharing the query-prefix KV
// cache. The template+query prefix is prefilled ONCE; each doc suffix is
// then forwarded at absolute positions [P, P+len) attending over the
// cached prefix. This is exactly the arithmetic of a single full forward
// (the rewind never reads stale K/V because attention only touches
// positions <= cached+s, and each doc overwrites its own region), so the
// scores are bit-identical to per-doc rerank() calls. For rerank_top docs
// the prefix work is paid once instead of rerank_top times.
std::vector<float> rerank_many(const Model& m, const std::string& query,
                               const std::vector<std::string>& docs) {
    std::lock_guard<std::mutex> lk(engine_mu());
    std::vector<float> scores(docs.size(), 0.0f);
    if (docs.empty()) return scores;
    std::vector<int> prefix = encode(m, build_rerank_prefix(query));
    if (prefix.empty()) {
        // Unreachable with the fixed template; fall back to per-doc rerank.
        for (std::size_t i = 0; i < docs.size(); i++)
            scores[i] = rerank_one(m, query, docs[i]);
        return scores;
    }
    KVState& st = rerank_kv();
    if (static_cast<int>(st.k.size()) != m.layers) {
        st = KVState();  // different model: full reset
        st.k.resize(m.layers);
        st.v.resize(m.layers);
    }
    st.pos = 0;
    forward_impl(m, prefix, &st);  // prefill the prefix once
    const int P = st.pos;
    const int H = m.hidden;
    const float* et = m.embed.data() + size_t(m.true_id) * H;
    const float* ef = m.embed.data() + size_t(m.false_id) * H;
    for (std::size_t i = 0; i < docs.size(); i++) {
        std::vector<int> ids = encode(m, build_rerank_suffix(docs[i]));
        if (ids.empty()) continue;
        st.pos = P;  // rewind to the prefix boundary for the next doc
        std::vector<float> h = forward_impl(m, ids, &st);
        const float* last = h.data() + size_t(ids.size() - 1) * H;
        float logit_true = 0.0f, logit_false = 0.0f;
        for (int j = 0; j < H; j++) {
            logit_true += last[j] * et[j];
            logit_false += last[j] * ef[j];
        }
        scores[i] = logit_true - logit_false;
    }
    return scores;
}

std::string generate(const Model& m, const std::string& prompt,
                     int max_new_tokens, float temperature,
                     bool no_leading_digit) {
    std::lock_guard<std::mutex> lk(engine_mu());
    if (m.lm_head.empty()) return "";
    auto ids = encode(m, prompt);
    if (ids.empty()) return "";  // empty prompt would index h[-H] below
    int H = m.hidden;
    int V = m.tokenizer_size;
    // First-step digit block: token ids whose text starts with a digit after
    // any leading whitespace ("2023", " 5", "1st", "1990s" ...). Built once,
    // consulted only at step 0; the scan is ~150K tiny strings, negligible
    // next to the prefill.
    std::vector<char> digit_lead(V, 0);
    if (no_leading_digit) {
        for (int i = 0; i < V; i++) {
            const std::string& t = m.id2tok[i];
            const std::size_t b = t.find_first_not_of(" \t");
            if (b != std::string::npos && t[b] >= '0' && t[b] <= '9')
                digit_lead[i] = 1;
        }
    }
    // Prefill: run the prompt once, filling the per-layer K/V cache. Each
    // decode step then processes ONE token over the cache instead of
    // re-running the whole sequence (~10-20x fewer FLOPs for long prompts).
    KVState state;
    state.k.resize(m.layers);
    state.v.resize(m.layers);
    std::vector<float> h = forward_impl(m, ids, &state);
    std::vector<float> logits(V);
    // S=1 GEMM scratch (BK*BN + S*BN); the lm_head goes through the batched
    // GEMM path so it gets SIMD + INT8 support.
    std::vector<float> scratch(64 * 8 + 8);
    std::vector<int> new_ids;
    for (int step = 0; step < max_new_tokens; step++) {
        const float* last = h.data() + (h.size() / H - 1) * H;
        // lm_head is a plain FP32 [vocab, hidden] matrix: S=1 GEMM (SIMD,
        // single-threaded) instead of a scalar V×H loop.
        gemm(std::span<const float>(last, H), 1, H, m.lm_head, V, logits,
             scratch);
        // Greedy for now (sampling is a later milestone).
        int best = 0;
        float bv = -1e30f;
        for (int i = 0; i < V; i++) {
            if (step == 0 && digit_lead[i]) continue;
            if (logits[i] > bv) { bv = logits[i]; best = i; }
        }
        if (best == m.eos) break;
        new_ids.push_back(best);
        std::vector<int> one(1, best);
        h = forward_impl(m, one, &state);
    }
    (void)temperature;
    return decode(m, new_ids);
}

}  // namespace qwen3
}  // namespace nyaxus
