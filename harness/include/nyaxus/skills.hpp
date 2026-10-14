#pragma once

#include <optional>
#include <string>
#include <vector>

namespace nyaxus {
namespace skills {

// One installed skill: a SKILL.md's frontmatter (name, description,
// disable-model-invocation) plus its instruction body. Multi-file skills
// ship auxiliary files (references, schemas, scripts) in `dir` beside the
// SKILL.md; the skill tool surfaces that directory to the model.
struct Skill {
    std::string name;
    std::string description;
    std::string content;  // the instruction body (after the frontmatter)
    std::string path;     // the SKILL.md path (provenance)
    std::string dir;      // the skill directory (the SKILL.md's parent)
    bool disable_model_invocation = false;  // user-invoked only
};

// Strict frontmatter validation (deliberate, permanent stance: loud
// errors, not warnings). A valid name is
// [a-z0-9-]{1,64}; a valid description is 1-1024 characters. Each returns
// an empty string when valid, else the reason for rejection.
std::string validate_name(const std::string& name);
std::string validate_description(const std::string& description);

// XML-escape one catalog/envelope value (& < > " ') - the prompt-injection
// defense: skill names, descriptions, and bodies are untrusted content.
std::string xml_escape(const std::string& text);

// The <skill_content> activation envelope:
// <skill_content name> + <source> ("project-local" or "global") +
// <directory> + the relative-path note + the auxiliary-file list (when
// non-empty) + the XML-escaped body. Escaping the whole body doubles as
// tag neutralization: a hostile body cannot emit a raw </skill_content>
// and break out of the wrapper.
std::string skill_envelope(const Skill& skill, const std::string& source,
                           const std::vector<std::string>& aux_files);

// The fixed total catalog budget in bytes (predictable for authors, not
// model-relative). catalog_block truncates past it with a note.
constexpr std::size_t kCatalogBudgetBytes = 50 * 1024;

// Parse one SKILL.md's text. Returns nullopt when the required frontmatter
// (name + description) is missing or fails strict validation. `path` is
// recorded for provenance.
std::optional<Skill> parse(const std::string& text, const std::string& path);

// Discover skills by scanning `roots` for SKILL.md files (bounded-recursive).
// Missing roots are skipped. Duplicate names resolve to the first root in the
// given order (callers put the project root before the global one so project
// skills shadow globals). When `rejected` is non-null, each skipped file is
// appended as "path (reason)" so callers can log it loudly.
std::vector<Skill> discover(const std::vector<std::string>& roots,
                            std::vector<std::string>* rejected = nullptr);

// The progressive-disclosure catalog: an intro line plus one line per
// model-invocable skill (name + description, XML-escaped). Skills with
// disable_model_invocation are excluded (user-invoked only). The block is
// capped at kCatalogBudgetBytes (overflowing skills are omitted with a
// note). Empty string when there are none.
std::string catalog_block(const std::vector<Skill>& skills);

// Bounded recursive listing of the files under one directory as slash-relative
// paths (dotfiles and dotdirs skipped; depth and entry count are bounded).
// Feeds the skill tool's auxiliary-file header.
std::vector<std::string> list_files(const std::string& dir);

}  // namespace skills
}  // namespace nyaxus
