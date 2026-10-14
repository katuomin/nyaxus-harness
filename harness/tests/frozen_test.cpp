// Frozen sections: marker parsing (none/one/multiple/nested/unterminated)
// and the survival check (kept/dropped/reordered/whitespace-only change).
// Design: docs/frozen-sections.md. Pure logic, no model artifacts.
#include <iostream>
#include <string>
#include <vector>

#include "nyaxus/frozen.hpp"

using namespace nyaxus;
namespace fz = nyaxus::frozen;

static int g_fail = 0;

static void check(bool cond, const std::string& what) {
    std::cout << (cond ? "  PASS  " : "  FAIL  ") << what << "\n";
    std::cout.flush();
    if (!cond) g_fail++;
}

static void section(const std::string& name) {
    std::cout << "\n== " << name << " ==\n";
    std::cout.flush();
}

int main() {
    // ── parse_sections: markers ───────────────────────────────────────────
    section("parse: no markers");
    {
        check(fz::parse_sections("plain text, no blocks").empty(),
              "no markers -> no spans");
        check(fz::parse_sections("").empty(), "empty text -> no spans");
        check(fz::parse_sections("<!-- not-a-frozen --> x <!-- /x -->")
                  .empty(),
              "other comment markers are not frozen markers");
    }

    section("parse: one block");
    {
        const auto spans =
            fz::parse_sections("before <!-- frozen -->pinned line<!-- /frozen -->after");
        check(spans.size() == 1, "exactly one span");
        check(spans.size() == 1 && spans[0] == "pinned line",
              "span is the inner text, markers excluded");
    }

    section("parse: multiple blocks");
    {
        const auto spans =
            fz::parse_sections("<!-- frozen -->A<!-- /frozen -->mid<!-- frozen -->B<!-- /frozen -->");
        check(spans.size() == 2, "two spans");
        check(spans.size() == 2 && spans[0] == "A" && spans[1] == "B",
              "spans in document order");
    }

    section("parse: nested blocks (innermost-first)");
    {
        const auto spans = fz::parse_sections(
            "<!-- frozen -->outer1<!-- frozen -->inner<!-- /frozen -->outer2<!-- /frozen -->");
        check(spans.size() == 2, "two spans (inner, then outer)");
        check(spans.size() == 2 && spans[0] == "inner",
              "the close ends the most recently opened block");
        check(spans.size() == 2 &&
                  spans[1] == "outer1<!-- frozen -->inner<!-- /frozen -->outer2",
              "the outer span keeps its inner markers verbatim");
    }

    section("parse: unterminated and stray markers (soft guard)");
    {
        check(fz::parse_sections("<!-- frozen -->never closed").empty(),
              "unterminated block is ignored");
        check(fz::parse_sections("<!-- /frozen -->").empty(),
              "stray closing marker is ignored");
        check(fz::parse_sections("<!-- frozen -->A<!-- /frozen --><!-- frozen -->B")
                  .size() == 1,
              "terminated block before an unterminated one is kept");
    }

    section("parse: empty and adjacent blocks");
    {
        const auto spans = fz::parse_sections("<!-- frozen --><!-- /frozen -->");
        check(spans.size() == 1 && spans[0].empty(),
              "adjacent markers -> one empty span");
        const auto multi = fz::parse_sections(
            "<!-- frozen -->A<!-- /frozen -->\n"
            "<!-- frozen -->\nline 1\nline 2\n<!-- /frozen -->");
        check(multi.size() == 2 && multi[1] == "\nline 1\nline 2\n",
              "inner text keeps its whitespace exactly");
    }

    // ── check: survival ───────────────────────────────────────────────────
    section("check: spans survive");
    {
        const auto r = fz::check("x pinned line y", {"pinned line"});
        check(r.ok && r.missing.empty() && r.reason.empty(),
              "span kept -> ok, no reason");
        const auto r2 =
            fz::check("<!-- frozen -->pinned line<!-- /frozen -->",
                      {"pinned line"});
        check(r2.ok, "survival inside the markers is fine");
        check(fz::check("anything", {}).ok, "no spans -> no constraint");
    }

    section("check: span dropped");
    {
        const auto r = fz::check("v2 core text", {"pinned line"});
        check(!r.ok && r.missing.size() == 1 && r.missing[0] == "pinned line",
              "dropped span -> not ok, listed in missing");
        check(!r.reason.empty() && r.reason.find("pinned line") != std::string::npos,
              "reason names the missing span");
    }

    section("check: order and surroundings are irrelevant");
    {
        const auto reordered =
            fz::check("Z pinned line A middle B", {"pinned line", "B"});
        check(reordered.ok, "spans in any order, any surroundings -> ok");
    }

    section("check: whitespace-only change is a miss (verbatim invariant)");
    {
        check(!fz::check("pinned  line", {"pinned line"}).ok,
              "extra space inside the span -> miss");
        check(!fz::check("Pinned line", {"pinned line"}).ok,
              "case change -> miss");
        check(!fz::check("pinned line\n", {"pinned line\n\n"}).ok,
              "trailing newline count change -> miss");
    }

    section("check: multiple misses all reported");
    {
        const auto r = fz::check("nothing here", {"one", "two"});
        check(!r.ok && r.missing.size() == 2, "both missing spans listed");
        check(r.reason.find("one") != std::string::npos &&
                  r.reason.find("two") != std::string::npos,
              "reason names every missing span");
    }

    section("check: round-trip (parse then check the same text)");
    {
        const std::string text =
            "voice module\n<!-- frozen -->\nI speak in short, dry sentences.\n"
            "<!-- /frozen -->\nmore text";
        const auto spans = fz::parse_sections(text);
        check(spans.size() == 1, "one declared span");
        check(fz::check(text, spans).ok, "the source text keeps its own span");
    }

    section("check: long span snippets stay bounded");
    {
        std::string long_span(200, 'x');
        const auto r = fz::check("y", {long_span});
        check(!r.ok && r.reason.size() < 300,
              "reason is bounded even for long spans");
    }

    std::cout << (g_fail == 0 ? "\nFROZEN_TEST_PASSED\n" : "\nFROZEN_TEST_FAILED\n");
    return g_fail == 0 ? 0 : 1;
}
