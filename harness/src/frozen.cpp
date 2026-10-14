// Frozen sections: the "these exact spans must survive" invariant for AI
// self-edits (design: docs/frozen-sections.md).
//
// parse_sections extracts the inner text of `<!-- frozen --> ...
// <!-- /frozen -->` blocks; check requires each extracted span to appear
// verbatim in the new content. Pure: same inputs, same verdict, no I/O.
#include "nyaxus/frozen.hpp"

namespace nyaxus {
namespace frozen {
namespace {

// Arrays (not char*) so sizeof gives the literal length.
constexpr char kOpen[] = "<!-- frozen -->";
constexpr char kClose[] = "<!-- /frozen -->";
constexpr std::size_t kOpenLen = sizeof(kOpen) - 1;
constexpr std::size_t kCloseLen = sizeof(kClose) - 1;

// Bounded snippet for the human-readable reason: newlines flattened,
// truncated with an ellipsis.
std::string snippet(const std::string& span) {
    std::string out = span.size() > 80 ? span.substr(0, 80) : span;
    for (char& c : out) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    if (span.size() > 80) out += "...";
    return out;
}

}  // namespace

std::vector<std::string> parse_sections(const std::string& text) {
    std::vector<std::string> spans;
    std::vector<std::size_t> open_stack;  // offset after each open marker
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t open_at = text.find(kOpen, i);
        const std::size_t close_at = text.find(kClose, i);
        if (open_at == std::string::npos && close_at == std::string::npos) {
            break;
        }
        const bool take_open =
            open_at != std::string::npos &&
            (close_at == std::string::npos || open_at < close_at);
        if (take_open) {
            open_stack.push_back(open_at + kOpenLen);
            i = open_at + kOpenLen;
        } else {
            if (open_stack.empty()) {
                // Stray closing marker: no block to end, skip it.
                i = close_at + kCloseLen;
                continue;
            }
            // Innermost-first: this close ends the most recent open block.
            const std::size_t start = open_stack.back();
            open_stack.pop_back();
            spans.push_back(text.substr(start, close_at - start));
            i = close_at + kCloseLen;
        }
    }
    // Blocks still open at EOF are unterminated: ignored (soft guard).
    return spans;
}

CheckResult check(const std::string& text,
                  const std::vector<std::string>& spans) {
    CheckResult result;
    for (const std::string& span : spans) {
        if (text.find(span) == std::string::npos) {
            result.missing.push_back(span);
        }
    }
    if (result.missing.empty()) return result;
    result.ok = false;
    result.reason = "frozen section(s) missing from the new content:";
    for (const std::string& span : result.missing) {
        result.reason += " \"" + snippet(span) + "\"";
    }
    result.reason +=
        ". The text between the <!-- frozen --> and <!-- /frozen --> "
        "markers must survive verbatim - keep it intact and retry.";
    return result;
}

}  // namespace frozen
}  // namespace nyaxus
