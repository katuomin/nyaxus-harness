#pragma once

#include <map>
#include <string>
#include <vector>

namespace nyaxus {
namespace promptpacks {

// One module declaration from the pack's manifest.json.
struct Module {
    std::string id;
    std::string path;
    std::string category;  // policy | identity | appearance | voice | relationship | continuity | memory-context | optional
    std::string slot;      // mutually-exclusive group name, empty = none
    bool required = false; // must appear in every plan that uses the pack
    bool reorderable = true;
    bool editable = true;
    std::vector<std::string> requires_;  // module ids that must accompany this one
};

struct Manifest {
    std::string pack;
    long long manifest_version = 0;
    std::vector<Module> modules;
    std::map<std::string, std::vector<std::string>> slots;  // slot name -> member ids
    bool ok = false;
    std::string parse_error;
};

// Parses manifest.json text. `ok` stays false on any structural problem.
Manifest parse_manifest(const std::string& json_text);

struct Report {
    bool valid = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    void error(std::string message) {
        valid = false;
        errors.push_back(std::move(message));
    }
    void warning(std::string message) { warnings.push_back(std::move(message)); }
};

// Validates the manifest against the pack's files (logical_path -> content).
// Covers: manifest presence/shape, module id/path rules, duplicate ids,
// category whitelist, slot membership, requires references, UTF-8, and
// per-file / total size limits.
Report validate_pack(const Manifest& manifest,
                     const std::map<std::string, std::string>& files);

// Validates a profile plan (ordered module ids) against the manifest:
// known ids, no duplicates, required modules present, at most one member per
// slot, and every `requires` dependency satisfied.
Report validate_plan(const Manifest& manifest, const std::vector<std::string>& plan);

struct ResolvedModule {
    std::string id;
    std::string path;
    std::string category;
    std::string content;
    long long bytes = 0;
    long long approx_tokens = 0;
    bool required = false;
};

struct Resolution {
    bool ok = false;
    std::string error;
    std::string prompt;  // the exact [PROMPT PACK] document
    std::vector<ResolvedModule> modules;
    long long total_bytes = 0;
    long long approx_tokens = 0;
    std::string fingerprint;  // SHA-256 of prompt
};

// Assembles the static prompt document for an ordered plan:
//
//   [PROMPT PACK]
//   pack: <name>
//   profile: <label>
//   revision: <n>
//
//   [MODULE <id>]
//   <content>
//
// The plan is validated first; on failure `ok` is false with the first error.
Resolution resolve(const Manifest& manifest,
                   const std::map<std::string, std::string>& files,
                   const std::vector<std::string>& plan,
                   const std::string& profile_label,
                   long long pack_revision);

// Rough local estimate (the provider-verified count is a later phase).
long long approx_tokens(long long bytes);

}  // namespace promptpacks
}  // namespace nyaxus
