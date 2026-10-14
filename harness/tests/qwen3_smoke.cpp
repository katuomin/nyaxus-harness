#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "nyaxus/qwen3.hpp"

using namespace nyaxus::qwen3;

static int g_fail = 0;

// Dump the embedding for an explicit id sequence (comma-separated) as
// space-separated floats, for oracle comparison.
static int dump_ids(const Model& m, const char* ids_arg) {
    std::vector<int> ids;
    std::stringstream ss(ids_arg);
    std::string tok;
    while (std::getline(ss, tok, ','))
        if (!tok.empty()) ids.push_back(std::stoi(tok));
    std::vector<float> v = embed_ids(m, ids);
    for (size_t i = 0; i < v.size(); i++)
        std::cout << v[i] << (i + 1 == v.size() ? "\n" : " ");
    return 0;
}

// Dump the embedding for encode(text) + [EOS], matching the oracle's input.
static int dump_text(const Model& m, const char* text) {
    std::vector<int> ids = encode(m, text);
    ids.push_back(m.eos);  // the oracle appends EOS before pooling
    std::vector<float> v = embed_ids(m, ids);
    for (size_t i = 0; i < v.size(); i++)
        std::cout << v[i] << (i + 1 == v.size() ? "\n" : " ");
    return 0;
}

static void check(bool cond, const std::string& what) {
    std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
    if (!cond) g_fail++;
}

static std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return v && *v ? std::string(v) : def;
}

int main(int argc, char** argv) {
    std::string embed_path =
        env_or("NYAXUS_EMBED_MODEL",
               "models/nyx/qwen3-embedding-0.6b-int8.nyx");
    if (argc >= 3 && (std::string(argv[1]) == "dumpids" ||
                      std::string(argv[1]) == "dumptext")) {
        std::string err;
        Model m = load(embed_path, err);
        if (!m.ok) {
            std::cerr << "load failed: " << err << "\n";
            return 2;
        }
        return std::string(argv[1]) == "dumpids" ? dump_ids(m, argv[2])
                                                 : dump_text(m, argv[2]);
    }
    std::string rerank_path =
        env_or("NYAXUS_RERANK_MODEL",
               "models/nyx/qwen3-reranker-0.6b-int8.nyx");

    std::string err;
    Model m = load(embed_path, err);
    if (!m.ok) {
        std::cout << "could not load embedding model: " << err << "\n";
        std::cout << "set NYAXUS_EMBED_MODEL to the .nyx path\n";
        return 2;
    }
    std::cout << "loaded " << embed_path << "\n";
    std::cout << "  layers=" << m.layers << " hidden=" << m.hidden
              << " heads=" << m.heads << "/" << m.kv_heads
              << " head_dim=" << m.head_dim << " inter=" << m.inter
              << " vocab=" << m.tokenizer_size << "\n";

    // ---- Tokenizer golden cases (ids verified against tokenizer.json) ----
    std::cout << "tokenizer:\n";
    auto e1 = encode(m, "hello");
    check(e1.size() == 1 && e1[0] == 14990, "encode('hello') == [14990] (got " +
                                                 std::to_string(e1.size()) +
                                                 (e1.size() ? ("[" + std::to_string(e1[0]) + "]") : "") + ")");
    auto e2 = encode(m, "the");
    check(e2.size() == 1 && e2[0] == 1782, "encode('the') == [1782]");
    auto e3 = encode(m, "world");
    check(e3.size() == 1 && e3[0] == 14615, "encode('world') == [14615]");
    auto e4 = encode(m, " ");
    check(e4.size() == 1 && e4[0] == 220, "encode(' ') == [220] (space)");
    auto d1 = decode(m, std::vector<int>{14990});
    check(d1 == "hello", "decode([14990]) == 'hello' (got '" + d1 + "')");

    // ---- Embedding ----
    std::cout << "embedding:\n";
    std::vector<float> v = embed(m, "hello");
    check(v.size() == 1024, "embedding dim == 1024 (got " +
                               std::to_string(v.size()) + ")");
    double ss = 0;
    for (float x : v) ss += double(x) * double(x);
    double norm = std::sqrt(ss);
    check(std::abs(norm - 1.0) < 1e-3, "embedding L2 norm ~= 1 (got " +
                                          std::to_string(norm) + ")");
    std::cout << "  first 8: ";
    for (int i = 0; i < 8; i++) std::cout << v[i] << " ";
    std::cout << "\n";

    std::vector<float> v2 = embed(m, "hello");
    bool same = v.size() == v2.size();
    if (same)
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] != v2[i]) { same = false; break; }
    check(same, "embedding is deterministic");

    // Similarity sanity: related > unrelated.
    std::vector<float> a = embed(m, "The capital of France is Paris");
    std::vector<float> b = embed(m, "Paris is the capital city of France");
    std::vector<float> c = embed(m, "Dogs are loyal four-legged animals");
    auto dot = [](const std::vector<float>& x, const std::vector<float>& y) {
        double s = 0;
        for (size_t i = 0; i < x.size() && i < y.size(); i++)
            s += double(x[i]) * double(y[i]);
        return s;
    };
    double sim_ab = dot(a, b);
    double sim_ac = dot(a, c);
    std::cout << "  sim(related)=" << sim_ab << " sim(unrelated)=" << sim_ac
              << "\n";
    check(sim_ab > sim_ac, "related sentence scores higher than unrelated");

    // ---- Reranker ----
    if (std::getenv("NYAXUS_SKIP_RERANK")) return g_fail ? 1 : 0;
    Model r = load(rerank_path, err);
    if (!r.ok) {
        std::cout << "note: reranker not loaded (" << err
                  << "); skipping rerank checks\n";
        return g_fail ? 1 : 0;
    }
    std::cout << "reranker:\n";
    struct Pair {
        const char* q;
        const char* good;
        const char* bad;
    };
    const Pair pairs[] = { {"What is the capital of France?",
                            "The capital of France is Paris.",
                            "Dogs are loyal four-legged animals."},
                           {"How do I reset a WiFi router?",
                            "Unplug the router for 30 seconds to reset it.",
                            "The recipe calls for two cups of flour."},
                           {"What year did World War II end?",
                            "World War II ended in 1945.",
                            "Cheese is a popular dairy product."} };
    for (const auto& p : pairs) {
        float g = rerank(r, p.q, p.good);
        float b = rerank(r, p.q, p.bad);
        std::cout << "  [" << p.q << "] relevant=" << g
                  << " irrelevant=" << b << "\n";
        check(g > b, std::string("relevant > irrelevant: ") + p.q);
    }
    float g0 = rerank(r, pairs[0].q, pairs[0].good);
    check(g0 > 0.0f, "relevant score is positive");

    // ---- rerank_many: shared query-prefix KV must not change the scores ----
    // The API prefills the template+query prefix once and scores each doc
    // suffix against the cache; the math must match per-doc rerank exactly.
    std::vector<std::string> docs2 = {pairs[0].good, pairs[0].bad};
    std::vector<float> many = rerank_many(r, pairs[0].q, docs2);
    check(many.size() == 2, "rerank_many returns one score per doc");
    float g1 = rerank(r, pairs[0].q, pairs[0].good);
    float b1 = rerank(r, pairs[0].q, pairs[0].bad);
    check(many[0] == g1 && many[1] == b1,
          "rerank_many bit-identical to per-doc rerank");
    check(many[0] > many[1], "rerank_many preserves relevance ordering");

    // ---- Reranker rank stability vs a reference model (quantization gate) ----
    // If a reference reranker is provided, the two models must agree on the
    // per-query top-1 doc, keep |score deltas| bounded, and rank the full
    // (query, doc) set in the same order (Spearman rho). This catches the
    // decision-boundary perturbation that cosine similarity on embeddings
    // cannot see.
    const char* ref_env = std::getenv("NYAXUS_RERANK_REF_MODEL");
    if (ref_env && *ref_env) {
        Model ref = load(ref_env, err);
        if (!ref.ok) {
            std::cout << "note: reference reranker not loaded (" << err
                      << "); skipping rank-stability checks\n";
        } else {
            std::cout << "rank stability vs " << ref_env << ":\n";
            struct QD {
                const char* q;
                const char* d;
            };
            const QD qds[] = {
                {"What is the capital of France?", "The capital of France is Paris."},
                {"What is the capital of France?", "Dogs are loyal four-legged animals."},
                {"What is the capital of France?", "The weather in Rome is usually sunny."},
                {"What is the capital of France?", "Flour is a key baking ingredient."},
                {"How do I reset a WiFi router?", "Unplug the router for 30 seconds to reset it."},
                {"How do I reset a WiFi router?", "The recipe calls for two cups of flour."},
                {"How do I reset a WiFi router?", "Paris is known for the Eiffel Tower."},
                {"How do I reset a WiFi router?", "Cheese is a popular dairy product."},
                {"What year did World War II end?", "World War II ended in 1945."},
                {"What year did World War II end?", "Dogs are loyal four-legged animals."},
                {"What year did World War II end?", "The recipe calls for two cups of flour."},
                {"What year did World War II end?", "Rome is the capital of Italy."},
            };
            const int nq = int(sizeof(qds) / sizeof(qds[0]));
            std::vector<float> sref(nq), scur(nq);
            bool delta_ok = true;
            for (int i = 0; i < nq; i++) {
                float a = rerank(ref, qds[i].q, qds[i].d);
                float b = rerank(r, qds[i].q, qds[i].d);
                sref[i] = a;
                scur[i] = b;
                double delta = std::abs(double(a) - double(b));
                if (delta > 0.5) {
                    delta_ok = false;
                    std::cout << "  [" << qds[i].q << " / " << qds[i].d
                              << "] ref=" << a << " cur=" << b << "\n";
                }
            }
            check(delta_ok, "all |score deltas| <= 0.5");

            // Per-query top-1 agreement.
            bool top1_ok = true;
            for (int q = 0; q < 3; q++) {
                int base = q * 4;
                int bi_ref = base, bi_cur = base;
                for (int i = 1; i < 4; i++) {
                    if (sref[base + i] > sref[bi_ref]) bi_ref = base + i;
                    if (scur[base + i] > scur[bi_cur]) bi_cur = base + i;
                }
                if (bi_ref != bi_cur) {
                    top1_ok = false;
                    std::cout << "  top-1 mismatch on query " << q + 1 << ": ref="
                              << qds[bi_ref].d << " cur=" << qds[bi_cur].d << "\n";
                }
            }
            check(top1_ok, "per-query top-1 doc agrees");

            // Spearman rho over the full pair set (Pearson on ranks).
            auto ranks = [](const std::vector<float>& s) {
                std::vector<double> rk(s.size());
                std::vector<int> idx(s.size());
                for (size_t i = 0; i < idx.size(); i++) idx[i] = int(i);
                std::sort(idx.begin(), idx.end(),
                          [&](int a, int b) { return s[a] < s[b]; });
                for (size_t i = 0; i < idx.size(); i++) rk[idx[i]] = double(i) + 1.0;
                return rk;
            };
            std::vector<double> rr = ranks(sref), rc = ranks(scur);
            double mean = (nq + 1) / 2.0, cov = 0.0, vr1 = 0.0, vr2 = 0.0;
            for (int i = 0; i < nq; i++) {
                double a = rr[i] - mean, b = rc[i] - mean;
                cov += a * b;
                vr1 += a * a;
                vr2 += b * b;
            }
            double rho = cov / std::sqrt(vr1 * vr2);
            std::cout << "  Spearman rho = " << rho << "\n";
            check(rho > 0.9, "Spearman rho > 0.9 (got " + std::to_string(rho) + ")");
        }
    }

    std::cout << (g_fail ? "\nFAILURES: " + std::to_string(g_fail) : "\nALL PASS")
              << "\n";
    return g_fail ? 1 : 0;
}
