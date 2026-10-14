#pragma once

#include <string>
#include <vector>

namespace nyaxus {
namespace frozen {

// Design: docs/frozen-sections.md. Author-declared frozen spans inside
// AI-editable text (prompt-pack modules, the journey document). A frozen
// span is the inner text of a `<!-- frozen --> ... <!-- /frozen -->`
// marker block: pinned voice lines, character facts that must survive any
// rewrite, verbatim.
//
// The guard is soft: text without terminated marker blocks imposes no
// constraint. The strength comes from the author declaring the spans, not
// from the guard finding them.

// Returns the inner text of each terminated marker block, in document
// order. Innermost-first pairing: a closing marker ends the most recently
// opened block. Blocks still open at EOF are unterminated and ignored
// (a soft guard cannot require a span whose end is unknown), as are
// closing markers with no open block.
std::vector<std::string> parse_sections(const std::string& text);

struct CheckResult {
    bool ok = true;
    std::vector<std::string> missing;  // spans not found verbatim
    std::string reason;  // human-readable, empty when ok
};

// Requires each span to survive verbatim in `text`. Order and surrounding
// text are irrelevant - only survival matters. On failure `reason`
// carries a bounded, human-readable summary of the missing spans,
// suitable for a tool result.
CheckResult check(const std::string& text,
                  const std::vector<std::string>& spans);

}  // namespace frozen
}  // namespace nyaxus
