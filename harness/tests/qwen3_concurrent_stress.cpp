// Concurrency stress for the in-process Qwen3 engine. The engine keeps
// process-wide non-thread-safe scratch (i16_scratch, rerank_kv) and a shared
// GemmPool; the daemon assumes it serializes engine calls. This hammers
// embed + rerank + generate from many threads with mixed sequence lengths
// (short = single-threaded GEMM path, long = parallel pool path) to expose
// any data race / re-entrancy. A glibc mutex assert (same-thread re-lock)
// aborts the process with a non-zero exit.
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "nyaxus/qwen3.hpp"

using namespace nyaxus::qwen3;

static std::atomic<int> g_iters{0};

static std::string make_text(int words) {
    std::string s;
    for (int i = 0; i < words; i++) s += "alpha beta gamma delta epsilon ";
    return s;
}

int main(int argc, char** argv) {
    std::string embed_path = "models/nyx/qwen3-embedding-0.6b-int8.nyx";
    std::string rerank_path = "models/nyx/qwen3-reranker-0.6b-int8.nyx";
    std::string title_path = "models/nyx/qwen3-title-0.6b-int8.nyx";
    if (argc >= 4) {
        embed_path = argv[1];
        rerank_path = argv[2];
        title_path = argv[3];
    }
    int nthreads = 8;
    if (const char* e = std::getenv("STRESS_THREADS")) nthreads = std::atoi(e);
    int per_thread = 40;
    if (const char* e = std::getenv("STRESS_ITERS")) per_thread = std::atoi(e);

    std::string err;
    Model emb = load(embed_path, err);
    if (!emb.ok) { std::cerr << "emb.load: " << err << "\n"; return 2; }
    Model rr = load(rerank_path, err);
    if (!rr.ok) { std::cerr << "rr.load: " << err << "\n"; return 2; }
    Model gen = load(title_path, err);
    bool have_title = gen.ok;
    if (!have_title) std::cerr << "note: title model not loaded (" << err << ")\n";

    std::cout << "starting " << nthreads << " threads x " << per_thread
              << " iters (embed+rerank+generate)\n";

    const std::string short_text = make_text(3);   // ~15 tokens: parallel
    const std::string long_text = make_text(60);   // ~300 tokens: parallel
    const std::string tiny_text = "hi";            // ~1 token: single-threaded
    const std::string query = "What is the capital of France?";
    std::vector<std::string> docs = {
        "The capital of France is Paris.",
        "Dogs are loyal four-legged animals.",
        "The recipe calls for two cups of flour."};

    std::vector<std::thread> threads;
    for (int t = 0; t < nthreads; t++) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < per_thread; i++) {
                int phase = (i + t) % 4;
                // embed: alternate tiny (single-threaded) / short / long
                if (phase == 0) (void)embed(emb, tiny_text);
                else if (phase == 1) (void)embed(emb, short_text);
                else (void)embed(emb, long_text);
                // rerank: single + many
                (void)rerank(rr, query, docs[(i + t) % docs.size()]);
                if (i % 3 == 0) (void)rerank_many(rr, query, docs);
                // generate (title): short prompt
                if (have_title)
                    (void)generate(gen, "Generate a title: " + short_text + "\nTitle: ", 8, 0.0f);
                g_iters++;
            }
        });
    }
    for (auto& th : threads) th.join();
    std::cout << "done: " << g_iters.load() << " engine calls, no crash\n";
    return 0;
}
