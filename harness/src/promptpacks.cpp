#include "nyaxus/promptpacks.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

#include "nyaxus/json.hpp"
#include "nyaxus/store.hpp"  // sha256_hex

namespace nyaxus {
namespace promptpacks {
namespace {

constexpr long long kMaxFileBytes = 256 * 1024;
constexpr long long kMaxPackBytes = 2 * 1024 * 1024;

bool valid_utf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0x00) return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        int extra = 0;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c == 0xE0) {
            extra = 2;
            lo = 0xA0;
        } else if (c >= 0xE1 && c <= 0xEC) extra = 2;
        else if (c == 0xED) {
            extra = 2;
            hi = 0x9F;  // no UTF-16 surrogates
        } else if (c >= 0xEE && c <= 0xEF) extra = 2;
        else if (c == 0xF0) {
            extra = 3;
            lo = 0x90;
        } else if (c >= 0xF1 && c <= 0xF3) extra = 3;
        else if (c == 0xF4) {
            extra = 3;
            hi = 0x8F;  // code points up to U+10FFFF
        } else {
            return false;
        }
        for (int k = 0; k < extra; ++k) {
            if (i + 1 + k >= text.size()) return false;
            const unsigned char cont = static_cast<unsigned char>(text[i + 1 + k]);
            if (cont < lo || cont > hi) return false;
        }
        i += 1 + static_cast<std::size_t>(extra);
    }
    return true;
}

bool valid_module_id(const std::string& id) {
    if (id.empty() || id.size() > 128) return false;
    std::size_t start = 0;
    while (start < id.size()) {
        const std::size_t dot = id.find('.', start);
        const std::size_t end = dot == std::string::npos ? id.size() : dot;
        if (end == start) return false;  // empty segment
        for (std::size_t i = start; i < end; ++i) {
            const char c = id[i];
            const bool letter = (c >= 'a' && c <= 'z');
            const bool digit = (c >= '0' && c <= '9');
            if (i == start) {
                if (!letter) return false;
            } else if (!letter && !digit && c != '_') {
                return false;
            }
        }
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return true;
}

bool valid_logical_path(const std::string& path) {
    if (path.empty() || path.size() > 256) return false;
    if (path.front() == '/' || path.back() == '/') return false;
    for (const char c : path) {
        if (c == '\\' || c == '\0') return false;
        if (static_cast<unsigned char>(c) < 0x20) return false;
    }
    std::size_t start = 0;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string segment = path.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

const std::set<std::string>& allowed_categories() {
    static const std::set<std::string> categories = {
        "policy", "identity", "appearance", "voice", "relationship",
        "continuity", "memory-context", "optional"};
    return categories;
}

const Module* find_module(const Manifest& manifest, const std::string& id) {
    for (const Module& module : manifest.modules) {
        if (module.id == id) return &module;
    }
    return nullptr;
}

}  // namespace

long long approx_tokens(long long bytes) {
    if (bytes <= 0) return 0;
    return (bytes + 3) / 4;
}

Manifest parse_manifest(const std::string& json_text) {
    Manifest manifest;
    json::Value root;
    try {
        root = json::parse(json_text);
    } catch (...) {
        manifest.parse_error = "manifest.json is not valid JSON";
        return manifest;
    }
    if (root.type() != json::Value::Type::Object) {
        manifest.parse_error = "manifest.json must be a JSON object";
        return manifest;
    }
    manifest.pack = root["pack"].as_string();
    manifest.manifest_version = root["manifest_version"].as_integer(0);
    if (!root.contains("modules") || root["modules"].type() != json::Value::Type::Array) {
        manifest.parse_error = "manifest.json must contain a modules array";
        return manifest;
    }
    for (const json::Value& entry : root["modules"].as_array()) {
        if (entry.type() != json::Value::Type::Object) {
            manifest.parse_error = "each module must be a JSON object";
            return manifest;
        }
        Module module;
        module.id = entry["id"].as_string();
        module.path = entry["path"].as_string();
        module.category = entry["category"].as_string();
        module.slot = entry["slot"].as_string();
        module.required = entry["required"].as_bool(false);
        module.reorderable = entry["reorderable"].as_bool(true);
        module.editable = entry["editable"].as_bool(true);
        if (entry.contains("requires") && entry["requires"].type() == json::Value::Type::Array) {
            for (const json::Value& dependency : entry["requires"].as_array()) {
                if (dependency.type() == json::Value::Type::String) {
                    module.requires_.push_back(dependency.as_string());
                }
            }
        }
        manifest.modules.push_back(std::move(module));
    }
    if (root.contains("slots") && root["slots"].type() == json::Value::Type::Object) {
        for (const auto& [slot_name, members] : root["slots"].as_object()) {
            if (members.type() != json::Value::Type::Array) {
                manifest.parse_error = "slot '" + slot_name + "' must be an array of module ids";
                return manifest;
            }
            std::vector<std::string> member_ids;
            for (const json::Value& member : members.as_array()) {
                if (member.type() != json::Value::Type::String) {
                    manifest.parse_error = "slot members must be module id strings";
                    return manifest;
                }
                member_ids.push_back(member.as_string());
            }
            manifest.slots[slot_name] = std::move(member_ids);
        }
    }
    manifest.ok = true;
    return manifest;
}

Report validate_pack(const Manifest& manifest,
                     const std::map<std::string, std::string>& files) {
    Report report;
    if (!manifest.ok) {
        report.error("manifest: " + manifest.parse_error);
        return report;
    }
    if (manifest.pack.empty()) {
        report.error("manifest: 'pack' name is required");
    }
    if (manifest.manifest_version != 1) {
        report.error("manifest: unsupported manifest_version " +
                     std::to_string(manifest.manifest_version) + " (expected 1)");
    }
    if (files.find("manifest.json") == files.end()) {
        report.error("pack must contain the manifest.json file");
    }

    // File hygiene: UTF-8 and size limits.
    long long total_bytes = 0;
    for (const auto& [path, content] : files) {
        total_bytes += static_cast<long long>(content.size());
        if (static_cast<long long>(content.size()) > kMaxFileBytes) {
            report.error(path + ": exceeds the per-file size limit (" +
                         std::to_string(kMaxFileBytes) + " bytes)");
        }
        if (!valid_utf8(content)) {
            report.error(path + ": content is not valid UTF-8");
        }
    }
    if (total_bytes > kMaxPackBytes) {
        report.error("pack total size exceeds the limit (" +
                     std::to_string(kMaxPackBytes) + " bytes)");
    }

    if (manifest.modules.empty()) {
        report.error("manifest: no modules declared");
        return report;
    }

    std::set<std::string> seen_ids;
    std::set<std::string> seen_paths;
    std::set<std::string> known_ids;
    for (const Module& module : manifest.modules) known_ids.insert(module.id);

    for (const Module& module : manifest.modules) {
        const std::string label = module.id.empty() ? "<unnamed module>" : module.id;
        if (!valid_module_id(module.id)) {
            report.error("module " + label + ": invalid id '" + module.id +
                         "' (lowercase segments of letters/digits/underscores, dot-separated)");
        }
        if (!seen_ids.insert(module.id).second) {
            report.error("duplicate module id '" + module.id + "'");
        }
        if (!valid_logical_path(module.path)) {
            report.error("module " + label + ": invalid logical path '" + module.path +
                         "' (relative, '/'-separated, no '.' or '..')");
        }
        if (module.path == "manifest.json") {
            report.error("module " + label + ": path collides with manifest.json");
        }
        if (!seen_paths.insert(module.path).second) {
            report.error("duplicate logical path '" + module.path + "'");
        }
        if (!module.path.empty() && files.find(module.path) == files.end()) {
            report.error("module " + label + ": path '" + module.path + "' is not in the pack");
        }
        if (!allowed_categories().count(module.category)) {
            report.error("module " + label + ": unknown category '" + module.category + "'");
        }
        if (!module.slot.empty()) {
            const auto slot = manifest.slots.find(module.slot);
            if (slot == manifest.slots.end()) {
                report.error("module " + label + ": undeclared slot '" + module.slot + "'");
            } else {
                const bool member =
                    std::find(slot->second.begin(), slot->second.end(), module.id) !=
                    slot->second.end();
                if (!member) {
                    report.error("module " + label + ": not a member of slot '" +
                                 module.slot + "'");
                }
            }
        }
        for (const std::string& dependency : module.requires_) {
            if (!known_ids.count(dependency)) {
                report.error("module " + label + ": requires unknown module '" +
                             dependency + "'");
            }
        }
    }

    // Slot membership both ways: every declared member must exist.
    for (const auto& [slot_name, members] : manifest.slots) {
        for (const std::string& member : members) {
            if (!known_ids.count(member)) {
                report.error("slot '" + slot_name + "': unknown member '" + member + "'");
            }
        }
    }

    // Files that no module references are allowed (extra reference material)
    // but worth surfacing so a typo'd path is noticed.
    for (const auto& [path, content] : files) {
        (void)content;
        if (path == "manifest.json") continue;
        bool referenced = false;
        for (const Module& module : manifest.modules) {
            if (module.path == path) referenced = true;
        }
        if (!referenced) {
            report.warning("file '" + path + "' is not referenced by any module");
        }
    }
    return report;
}

Report validate_plan(const Manifest& manifest, const std::vector<std::string>& plan) {
    Report report;
    if (!manifest.ok) {
        report.error("manifest: " + manifest.parse_error);
        return report;
    }
    if (plan.empty()) {
        report.error("plan is empty");
        return report;
    }
    std::set<std::string> in_plan;
    for (const std::string& id : plan) {
        if (!in_plan.insert(id).second) {
            report.error("module '" + id + "' appears more than once in the plan");
        }
        if (!find_module(manifest, id)) {
            report.error("plan references unknown module '" + id + "'");
        }
    }
    for (const Module& module : manifest.modules) {
        if (module.required && !in_plan.count(module.id)) {
            report.error("required module '" + module.id + "' is missing from the plan");
        }
    }
    for (const auto& [slot_name, members] : manifest.slots) {
        int present = 0;
        for (const std::string& member : members) {
            if (in_plan.count(member)) ++present;
        }
        if (present > 1) {
            report.error("slot '" + slot_name + "': plan contains " +
                         std::to_string(present) + " members (at most one)");
        }
    }
    for (const std::string& id : plan) {
        const Module* module = find_module(manifest, id);
        if (!module) continue;
        for (const std::string& dependency : module->requires_) {
            if (!in_plan.count(dependency)) {
                report.error("module '" + id + "' requires '" + dependency +
                             "', which is not in the plan");
            }
        }
    }
    return report;
}

Resolution resolve(const Manifest& manifest,
                   const std::map<std::string, std::string>& files,
                   const std::vector<std::string>& plan,
                   const std::string& profile_label,
                   long long pack_revision) {
    Resolution resolution;
    const Report report = validate_plan(manifest, plan);
    if (!report.valid) {
        std::ostringstream joined;
        for (std::size_t i = 0; i < report.errors.size(); ++i) {
            if (i) joined << "; ";
            joined << report.errors[i];
        }
        resolution.error = joined.str();
        return resolution;
    }
    std::ostringstream doc;
    doc << "[PROMPT PACK]\n";
    doc << "pack: " << manifest.pack << "\n";
    doc << "profile: " << (profile_label.empty() ? "unspecified" : profile_label) << "\n";
    doc << "revision: " << pack_revision << "\n\n";
    long long total_bytes = 0;
    for (const std::string& id : plan) {
        const Module* module = find_module(manifest, id);
        if (!module) continue;  // validate_plan already rejected this
        const auto file = files.find(module->path);
        if (file == files.end()) {
            resolution.error = "module '" + id + "': file missing at resolve time";
            return resolution;
        }
        std::string content = file->second;
        if (!content.empty() && content.back() != '\n') content.push_back('\n');
        ResolvedModule resolved;
        resolved.id = module->id;
        resolved.path = module->path;
        resolved.category = module->category;
        resolved.content = content;
        resolved.bytes = static_cast<long long>(content.size());
        resolved.approx_tokens = approx_tokens(resolved.bytes);
        resolved.required = module->required;
        total_bytes += resolved.bytes;
        doc << "[MODULE " << module->id << "]\n" << content << "\n";
        resolution.modules.push_back(std::move(resolved));
    }
    resolution.prompt = doc.str();
    resolution.total_bytes = total_bytes;
    resolution.approx_tokens = approx_tokens(total_bytes);
    resolution.fingerprint = sha256_hex(resolution.prompt);
    resolution.ok = true;
    return resolution;
}

}  // namespace promptpacks
}  // namespace nyaxus
