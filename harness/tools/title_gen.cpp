// Engine-side client for the title model: one title per prompt.
//
//   title_gen.exe <title_model.nyx> <prompts.bin> <out.bin>
//
// prompts.bin / out.bin are NUL-separated UTF-8 strings (a trailing NUL on
// input is optional). Output has exactly one entry per input prompt, in
// order; a prompt the engine can't title comes back as an empty string.
//
// Greedy decode, 24 tokens - must match MAX_NEW_TOKENS in
// tools/finetune_title.py, or the engine spot-check won't line up with the
// HF eval.
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "nyaxus/qwen3.hpp"

using namespace nyaxus::qwen3;

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: title_gen.exe <title_model.nyx> <prompts.bin> <out.bin>\n");
        return 2;
    }
    std::string err;
    Model m = load(argv[1], err);
    if (!m.ok) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }

    std::ifstream in(argv[2], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 1;
    }
    std::string blob((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    std::string out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = blob.find('\0', start);
        const std::string p = (pos == std::string::npos)
                                  ? blob.substr(start)
                                  : blob.substr(start, pos - start);
        if (!p.empty()) {
            // Mirror the daemon title policy: the prompt is
            // "Generate a title: {message}\nTitle: "; block digit-leading
            // first tokens when the message carries no digits, so engine
            // scores match what the daemon ships.
            bool no_leading_digit = true;
            const std::size_t b = p.find("Generate a title: ");
            const std::size_t e = p.find("\nTitle: ");
            if (b != std::string::npos && e != std::string::npos) {
                for (std::size_t i = b + 18; i < e; i++)
                    if (p[i] >= '0' && p[i] <= '9') {
                        no_leading_digit = false;
                        break;
                    }
            }
            out += generate(m, p, 24, 0.0f, no_leading_digit);
        }
        out += '\0';
        if (pos == std::string::npos)
            break;
        start = pos + 1;
        if (start >= blob.size())
            break;  // trailing NUL - done
    }

    std::ofstream o(argv[3], std::ios::binary);
    if (!o) {
        std::fprintf(stderr, "cannot open %s\n", argv[3]);
        return 1;
    }
    o.write(out.data(), static_cast<std::streamsize>(out.size()));
    return 0;
}
