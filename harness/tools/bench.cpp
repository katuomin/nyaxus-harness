// Timing benchmark for the Qwen3 engine: embed + rerank at several lengths.
//
// Host-friendly: takes the two .nyx model paths as positional args, prints a
// CPU-feature header (so we know which SIMD path is active), and is built
// with a static runtime so it runs from a plain cmd with no DLL setup.
//
//   bench.exe <embed_model.nyx> <rerank_model.nyx>
//
// (Env vars NYAXUS_EMBED_MODEL / NYAXUS_RERANK_MODEL still work as a fallback.)
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "nyaxus/qwen3.hpp"

using namespace nyaxus::qwen3;

extern "C" int nyaxus_cpu_has_avx_fma();

static std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}

static unsigned cpuid_bit(int leaf, int subleaf, int reg, int bit) {
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(leaf), "c"(subleaf));
    unsigned v = (reg == 0 ? eax : reg == 1 ? ebx : reg == 2 ? ecx : edx);
    return (v >> bit) & 1u;
}

// Build a sentence of roughly n words (each ~1 token for short ASCII words).
static std::string words(int n) {
    const char* pool[] = {"alpha", "bravo", "charlie", "delta", "echo",
                          "foxtrot", "golf", "hotel", "india", "juliet"};
    std::string s;
    for (int i = 0; i < n; i++) {
        s += pool[i % 10];
        if (i + 1 < n) s += " ";
    }
    return s;
}

int main(int argc, char** argv) {
    // --- CPU feature header ---
    unsigned max_leaf = 0;
    __asm__ __volatile__("cpuid" : "=a"(max_leaf) : "a"(0));
    bool sse2 = true;  // x86-64 baseline
    bool avx = cpuid_bit(1, 0, 2, 28);
    bool fma = cpuid_bit(1, 0, 2, 12);
    // AVX2 is leaf 7, EBX bit 5 (reg=1).
    bool avx2 = max_leaf >= 7 && cpuid_bit(7, 0, 1, 5);
    int use_avx = nyaxus_cpu_has_avx_fma();
    printf("=== Qwen3 engine benchmark ===\n");
    printf("cpu: sse2=%d avx=%d avx2=%d fma=%d  ->  gemm path: %s, threads: %d\n",
           (int)sse2, (int)avx, (int)avx2, (int)fma,
           use_avx ? "AVX+FMA (256-bit)" : "SSE2 (128-bit)",
           engine_thread_count());
    fflush(stdout);

    // --- Model paths (positional args, env fallback) ---
    std::string embed_path =
        (argc >= 2) ? argv[1] : env_or("NYAXUS_EMBED_MODEL", "");
    std::string rerank_path =
        (argc >= 3) ? argv[2] : env_or("NYAXUS_RERANK_MODEL", "");
    if (embed_path.empty() || rerank_path.empty()) {
        fprintf(stderr,
                "usage: bench.exe <embed_model.nyx> <rerank_model.nyx>\n");
        return 2;
    }

    std::string err;
    printf("loading embed model: %s\n", embed_path.c_str());
    fflush(stdout);
    Model m = load(embed_path, err);
    if (!m.ok) {
        fprintf(stderr, "embed load failed: %s\n", err.c_str());
        return 2;
    }
    printf("loading rerank model: %s\n", rerank_path.c_str());
    fflush(stdout);
    Model r = load(rerank_path, err);
    if (!r.ok) {
        fprintf(stderr, "rerank load failed: %s\n", err.c_str());
        return 2;
    }

    // 3 warm-ups + 5 timed runs, report the median: single runs swing
    // ±20% on the small cases (thread wake-up, cache state), so the median
    // is the number to compare across builds.
    auto bench = [](const char* label, auto fn) {
        for (int w = 0; w < 3; w++) fn();
        std::vector<double> runs;
        for (int r = 0; r < 5; r++) {
            auto t0 = std::chrono::steady_clock::now();
            fn();
            auto t1 = std::chrono::steady_clock::now();
            runs.push_back(
                std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(runs.begin(), runs.end());
        double ms = runs[runs.size() / 2];
        printf("%-28s %10.1f ms (median of 5)\n", label, ms);
        fflush(stdout);
        return ms;
    };

    printf("\n--- embedding (append EOS, last-token pool) ---\n");
    for (int n : {11, 50, 100, 250}) {
        std::string text = words(n);
        int tok = int(encode(m, text).size()) + 1;  // + EOS
        char label[64];
        snprintf(label, sizeof(label), "embed %3d tok", tok);
        bench(label, [&] { embed(m, text); });
    }

    printf("\n--- reranker (query + doc, single logit diff) ---\n");
    std::string query = "What is the capital of France?";
    for (int n : {50, 100, 250}) {
        std::string doc = words(n);
        // Full prompt length (template + query + doc) - the actual forward.
        int tok = rerank_prompt_tokens(r, query, doc);
        char label[64];
        snprintf(label, sizeof(label), "rerank %3d tok", tok);
        bench(label, [&] { rerank(r, query, doc); });
    }

    printf("\n--- reranker many-doc (shared query prefix, 8 docs) ---\n");
    // Distinct docs (same length): a future quality check on the bench
    // wouldn't be able to hide ordering bugs in identical inputs.
    const char* lead[8] = {"alpha", "bravo", "charlie", "delta", "echo",
                           "foxtrot", "golf", "hotel"};
    std::vector<std::string> docs;
    for (int i = 0; i < 8; i++)
        docs.push_back(std::string(lead[i]) + " " + words(99));
    int tok = rerank_prompt_tokens(r, query, docs[0]);
    char label[64];
    snprintf(label, sizeof(label), "rerank-many 8x%3d tok", tok);
    bench(label, [&] { rerank_many(r, query, docs); });

    printf("\ndone.\n");
    return 0;
}
