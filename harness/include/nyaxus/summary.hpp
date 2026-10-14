#pragma once

#include <set>
#include <string>
#include <vector>

namespace nyaxus {
namespace summary {

// One validated compact-context document (schema v2).
//
// v2 moves the auxiliary output contract from strict JSON to a sectioned
// markdown checkpoint. The model fills the semantic fields; the harness fills the
// deterministic ones (anchors, latest_user_request) from the source so
// provenance no longer depends on the small model emitting citations.
struct Document {
    std::string goal;  // the user's overall intent / primary request
    std::vector<std::string> decisions;
    std::vector<std::string> constraints;
    std::vector<std::string> unresolved_tasks;
    std::vector<std::string> relevant_artifacts;
    std::vector<std::string> next_steps;
    std::string latest_user_request;  // verbatim most recent user message
    std::vector<std::string> anchors;  // mechanical: paths/urls from source
};

// v2 schema bounds. A compact summary is injected into every later primary
// request, so it must stay small and predictable.
constexpr std::size_t kMaxItemsPerField = 64;
constexpr std::size_t kMaxItemBytes = 4096;
constexpr std::size_t kMaxDocumentBytes = 64 * 1024;

// Source-document bounds for the auxiliary input channel. Oversized tool
// payloads and attachments are bounded into references instead of inlined.
constexpr std::size_t kMaxMessageContentBytes = 16 * 1024;
constexpr std::size_t kMaxAttachmentInlineBytes = 8 * 1024;

// Hierarchical chunking bounds. Beyond this the operator must use a
// selected fork instead of an unbounded summary.
constexpr std::size_t kMaxChunks = 8;

// Parse and validate raw auxiliary output into a Document. Accepts a
// sectioned markdown checkpoint (primary) or a JSON object (fallback), with
// a tolerant element normalizer. Succeeds when at least one field is
// non-empty; otherwise `error` carries a specific reason.
bool parse_document(const std::string& raw, Document& out, std::string& error);

// Any citation present in a model-generated field must reference a real
// source message id in `valid_ids`. Citations are optional; fabricated
// provenance (unknown ids) is rejected. Harness-populated fields (anchors,
// latest_user_request) are verbatim and not citation-checked.
bool validate_provenance(const Document& doc,
                         const std::set<long long>& valid_ids,
                         std::string& error);

// Deterministic compact JSON (keys sorted) suitable for storage, merge
// input, and round-tripping.
std::string canonicalize(const Document& doc);

// Human/model-readable sectioned rendering for injection into later primary
// requests. Reads better to the primary model than raw JSON.
std::string render_readable(const Document& doc);

// Deterministically harvest high-precision anchors (URLs and file paths) from
// a source transcript. Never paraphrased, so never hallucinated; this is the
// model-independent provenance that replaces per-claim citations.
void harvest_anchors(const std::string& source, std::vector<std::string>& out);

}  // namespace summary
}  // namespace nyaxus
