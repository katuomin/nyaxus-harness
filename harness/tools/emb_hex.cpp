// Bit-exactness oracle: prints the embedding vector of a fixed string, one
// value per line at full FP32 precision. Run with NYAXUS_ENGINE_THREADS=1
// and with the default pool size; the outputs must be byte-identical.
#include <cstdio>
#include <string>

#include "nyaxus/qwen3.hpp"

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: emb_hex <model.nyx> <text>\n");
        return 2;
    }
    std::string err;
    auto m = nyaxus::qwen3::load(argv[1], err);
    if (!m.ok) {
        fprintf(stderr, "load failed: %s\n", err.c_str());
        return 2;
    }
    auto v = nyaxus::qwen3::embed(m, argv[2]);
    for (float x : v) printf("%.9e\n", x);
    return 0;
}
