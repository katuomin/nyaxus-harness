// Prompt packs: durable pack/file storage, manifest validation, plan
// validation, and exact-prompt resolution.
//
// No model artifacts needed - the resolver is pure text assembly over the
// store, so this runs against a throwaway nyaxus.db in a temp directory.
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "nyaxus/json.hpp"
#include "nyaxus/promptpacks.hpp"
#include "nyaxus/store.hpp"

using namespace nyaxus;
namespace packs = nyaxus::promptpacks;

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

// A minimal valid pack: one required shared policy, one identity, one
// optional voice, and a two-member appearance slot.
static const char* kManifest =
    "{"
    "  \"pack\": \"test-characters\","
    "  \"manifest_version\": 1,"
    "  \"slots\": { \"test.appearance\": [\"test.appearance_a\", \"test.appearance_b\"] },"
    "  \"modules\": ["
    "    {\"id\": \"shared.policy\", \"path\": \"shared/policy.md\", \"category\": \"policy\","
    "     \"owner\": \"prompt_pack\", \"required\": true, \"reorderable\": false, \"editable\": true},"
    "    {\"id\": \"test.core\", \"path\": \"test/CORE.md\", \"category\": \"identity\","
    "     \"owner\": \"prompt_pack\", \"required\": false, \"reorderable\": false, \"editable\": true},"
    "    {\"id\": \"test.appearance_a\", \"path\": \"test/appearance-a.md\", \"category\": \"appearance\","
    "     \"slot\": \"test.appearance\", \"owner\": \"prompt_pack\", \"required\": false, \"reorderable\": false, \"editable\": true},"
    "    {\"id\": \"test.appearance_b\", \"path\": \"test/appearance-b.md\", \"category\": \"appearance\","
    "     \"slot\": \"test.appearance\", \"owner\": \"prompt_pack\", \"required\": false, \"reorderable\": false, \"editable\": true},"
    "    {\"id\": \"test.voice\", \"path\": \"test/voice.md\", \"category\": \"voice\","
    "     \"owner\": \"prompt_pack\", \"required\": false, \"reorderable\": true, \"editable\": true},"
    "    {\"id\": \"test.intimacy\", \"path\": \"test/intimacy.md\", \"category\": \"optional\","
    "     \"requires\": [\"shared.intimacy\"], \"owner\": \"prompt_pack\", \"required\": false, \"reorderable\": false, \"editable\": true},"
    "    {\"id\": \"shared.intimacy\", \"path\": \"shared/intimacy.md\", \"category\": \"policy\","
    "     \"owner\": \"prompt_pack\", \"required\": false, \"reorderable\": false, \"editable\": true}"
    "  ]"
    "}";

static std::map<std::string, std::string> sample_files() {
    std::map<std::string, std::string> files;
    files["manifest.json"] = kManifest;
    files["shared/policy.md"] = "# shared policy\nalways first\n";
    files["test/CORE.md"] = "# test core\nidentity\n";
    files["test/appearance-a.md"] = "# appearance a\n";
    files["test/appearance-b.md"] = "# appearance b\n";
    files["test/voice.md"] = "# voice\n";
    files["test/intimacy.md"] = "# test intimacy\n";
    files["shared/intimacy.md"] = "# shared intimacy\n";
    return files;
}

int main() {
    // ── store ──────────────────────────────────────────────────────────────
    section("store: pack and file CRUD");
    const std::filesystem::path db_path =
        std::filesystem::temp_directory_path() / "nyaxus_promptpacks_test.db";
    std::error_code ec;
    std::filesystem::remove(db_path, ec);
    SqliteStore store(db_path);

    check(store.create_pack("chars", "Characters", "test pack"), "create_pack");
    check(!store.create_pack("chars", "dup", "dup"), "duplicate create rejected");
    store.upsert_pack_file("chars", "shared/policy.md", "policy", sha256_hex("policy"));
    store.upsert_pack_file("chars", "test/CORE.md", "core", sha256_hex("core"));
    store.upsert_pack_file("chars", "manifest.json", "{}", sha256_hex("{}"));

    const auto packs = store.list_packs();
    check(packs.size() == 1 && packs[0].id == "chars", "list_packs");
    const auto pack = store.get_pack("chars");
    check(pack.has_value() && pack->name == "Characters", "get_pack");
    const auto files = store.list_pack_files("chars");
    check(files.size() == 3, "list_pack_files");
    // Path-sorted for stable fingerprints.
    check(files.size() == 3 && files[0].logical_path == "manifest.json" &&
              files[1].logical_path == "shared/policy.md" &&
              files[2].logical_path == "test/CORE.md",
          "list_pack_files is path-sorted");

    const long long rev_before = pack->revision;
    store.upsert_pack_file("chars", "shared/policy.md", "policy v2", sha256_hex("policy v2"));
    const auto file = store.get_pack_file("chars", "shared/policy.md");
    check(file.has_value() && file->content == "policy v2" && file->revision == 2,
          "file upsert bumps file revision");
    store.bump_pack_revision("chars");
    const auto pack_after = store.get_pack("chars");
    check(pack_after->revision == rev_before + 1, "bump_pack_revision");
    check(pack_after->validation_status == "unchecked", "revision reset validation to unchecked");

    store.set_pack_validation("chars", "valid", "fp-1");
    check(store.get_pack("chars")->validation_status == "valid" &&
              store.get_pack("chars")->fingerprint == "fp-1",
          "set_pack_validation");

    check(store.delete_pack_file("chars", "shared/policy.md"), "delete_pack_file");
    check(!store.delete_pack_file("chars", "missing.md"), "delete missing file rejected");
    check(!store.profile_references_pack("nobody"), "unreferenced pack guard is false");

    // Profile carries the pack association.
    StoredProfile profile;
    profile.id = "test-profile";
    profile.name = "Test Profile";
    profile.prompt_pack_id = "chars";
    profile.prompt_plan_json = json::stringify(json::Value(json::Array{json::Value("a.b")}));
    profile.created_at = "t";
    profile.updated_at = "t";
    profile.revision = 1;
    store.upsert_profile(profile);
    const auto loaded = store.get_profile("test-profile");
    check(loaded.has_value() && loaded->prompt_pack_id == "chars" &&
              loaded->prompt_plan_json == profile.prompt_plan_json,
          "profile round-trips prompt_pack_id and prompt_plan_json");
    check(store.profile_references_pack("chars"), "profile_references_pack");

    check(store.delete_pack("chars"), "delete_pack");
    check(!store.get_pack("chars").has_value(), "pack gone after delete");
    check(store.list_pack_files("chars").empty(), "files gone after delete");

    // ── manifest parsing ───────────────────────────────────────────────────
    section("manifest: parsing");
    const auto manifest = packs::parse_manifest(kManifest);
    check(manifest.ok && manifest.pack == "test-characters" && manifest.manifest_version == 1,
          "parse valid manifest");
    check(manifest.modules.size() == 7, "module count");
    check(manifest.slots.count("test.appearance") == 1 &&
              manifest.slots.at("test.appearance").size() == 2,
          "slot declared");
    const auto broken = packs::parse_manifest("{not json");
    check(!broken.ok && !broken.parse_error.empty(), "broken JSON rejected");
    const auto no_modules = packs::parse_manifest("{\"pack\": \"x\", \"manifest_version\": 1}");
    check(!no_modules.ok, "missing modules rejected");

    // ── pack validation ────────────────────────────────────────────────────
    section("validate_pack");
    const auto good = packs::validate_pack(manifest, sample_files());
    check(good.valid, "sample pack is valid");

    auto no_manifest_files = sample_files();
    no_manifest_files.erase("manifest.json");
    check(!packs::validate_pack(manifest, no_manifest_files).valid,
          "missing manifest.json is an error");

    {
        auto files = sample_files();
        files["bad/../escape.md"] = "x";
        const auto report = packs::validate_pack(manifest, files);
        // The manifest itself is fine; the extra file is only a warning.
        check(report.valid && !report.warnings.empty(),
              "unreferenced file is a warning, not an error");
    }
    {
        // A manifest whose module path escapes is an error.
        const std::string bad_manifest =
            "{\"pack\": \"x\", \"manifest_version\": 1, \"modules\": ["
            "{\"id\": \"a.b\", \"path\": \"../escape.md\", \"category\": \"policy\"}]}";
        const auto m = packs::parse_manifest(bad_manifest);
        std::map<std::string, std::string> files;
        files["manifest.json"] = bad_manifest;
        files["../escape.md"] = "x";
        check(!packs::validate_pack(m, files).valid, "escaping path rejected");
    }
    {
        const std::string dup_manifest =
            "{\"pack\": \"x\", \"manifest_version\": 1, \"modules\": ["
            "{\"id\": \"a.b\", \"path\": \"a.md\", \"category\": \"policy\"},"
            "{\"id\": \"a.b\", \"path\": \"b.md\", \"category\": \"policy\"}]}";
        const auto m = packs::parse_manifest(dup_manifest);
        std::map<std::string, std::string> files;
        files["manifest.json"] = dup_manifest;
        files["a.md"] = "x";
        files["b.md"] = "x";
        check(!packs::validate_pack(m, files).valid, "duplicate module id rejected");
    }
    {
        const std::string missing_file_manifest =
            "{\"pack\": \"x\", \"manifest_version\": 1, \"modules\": ["
            "{\"id\": \"a.b\", \"path\": \"absent.md\", \"category\": \"policy\"}]}";
        const auto m = packs::parse_manifest(missing_file_manifest);
        std::map<std::string, std::string> files;
        files["manifest.json"] = missing_file_manifest;
        check(!packs::validate_pack(m, files).valid, "module path not in pack rejected");
    }
    {
        const std::string bad_category =
            "{\"pack\": \"x\", \"manifest_version\": 1, \"modules\": ["
            "{\"id\": \"a.b\", \"path\": \"a.md\", \"category\": \"vibes\"}]}";
        const auto m = packs::parse_manifest(bad_category);
        std::map<std::string, std::string> files;
        files["manifest.json"] = bad_category;
        files["a.md"] = "x";
        check(!packs::validate_pack(m, files).valid, "unknown category rejected");
    }
    {
        // requires: a dependency that does not exist.
        const std::string req_manifest =
            "{\"pack\": \"x\", \"manifest_version\": 1, \"modules\": ["
            "{\"id\": \"a.b\", \"path\": \"a.md\", \"category\": \"optional\", \"requires\": [\"ghost.module\"]}]}";
        const auto m = packs::parse_manifest(req_manifest);
        std::map<std::string, std::string> files;
        files["manifest.json"] = req_manifest;
        files["a.md"] = "x";
        check(!packs::validate_pack(m, files).valid, "requires unknown module rejected");
    }
    {
        // Slot member that is not a declared module.
        const std::string slot_manifest =
            "{\"pack\": \"x\", \"manifest_version\": 1, \"slots\": {\"s\": [\"ghost.module\"]},"
            "\"modules\": [{\"id\": \"a.b\", \"path\": \"a.md\", \"category\": \"policy\"}]}";
        const auto m = packs::parse_manifest(slot_manifest);
        std::map<std::string, std::string> files;
        files["manifest.json"] = slot_manifest;
        files["a.md"] = "x";
        check(!packs::validate_pack(m, files).valid, "slot member not a module rejected");
    }
    {
        // NUL byte is not valid UTF-8. Explicit length: the string-literal
        // constructor would stop at the embedded NUL.
        auto files = sample_files();
        files["test/voice.md"] = std::string("bad\0byte", 8);
        check(!packs::validate_pack(manifest, files).valid, "NUL byte rejected");
    }

    // ── plan validation ────────────────────────────────────────────────────
    section("validate_plan");
    const std::vector<std::string> good_plan = {
        "shared.policy", "test.core", "test.appearance_a", "test.voice"};
    check(packs::validate_plan(manifest, good_plan).valid, "good plan valid");

    check(!packs::validate_plan(manifest, {}).valid, "empty plan rejected");
    {
        std::vector<std::string> plan = {"shared.policy", "test.core", "ghost.module"};
        check(!packs::validate_plan(manifest, plan).valid, "unknown module rejected");
    }
    {
        std::vector<std::string> plan = {"shared.policy", "test.core", "test.core"};
        check(!packs::validate_plan(manifest, plan).valid, "duplicate module rejected");
    }
    {
        std::vector<std::string> plan = {"test.core"};  // shared.policy is required
        check(!packs::validate_plan(manifest, plan).valid, "missing required module rejected");
    }
    {
        std::vector<std::string> plan = {
            "shared.policy", "test.core", "test.appearance_a", "test.appearance_b"};
        check(!packs::validate_plan(manifest, plan).valid, "two slot members rejected");
    }
    {
        std::vector<std::string> plan = {"shared.policy", "test.core", "test.intimacy"};
        check(!packs::validate_plan(manifest, plan).valid,
              "requires dependency missing rejected");
        plan.push_back("shared.intimacy");
        check(packs::validate_plan(manifest, plan).valid, "requires dependency satisfied");
    }

    // ── resolution ─────────────────────────────────────────────────────────
    section("resolve");
    const auto resolution =
        packs::resolve(manifest, sample_files(), good_plan, "test-profile", 7);
    check(resolution.ok, "resolution ok");
    check(resolution.prompt.find("[PROMPT PACK]") == 0, "document header first");
    check(resolution.prompt.find("pack: test-characters") != std::string::npos, "pack name in header");
    check(resolution.prompt.find("profile: test-profile") != std::string::npos, "profile label in header");
    check(resolution.prompt.find("revision: 7") != std::string::npos, "revision in header");
    check(resolution.modules.size() == 4, "module count in resolution");
    check(resolution.modules[0].id == "shared.policy" && resolution.modules[0].required,
          "plan order preserved; required flag carried");
    const std::size_t pos_policy = resolution.prompt.find("[MODULE shared.policy]");
    const std::size_t pos_core = resolution.prompt.find("[MODULE test.core]");
    const std::size_t pos_voice = resolution.prompt.find("[MODULE test.voice]");
    check(pos_policy != std::string::npos && pos_core != std::string::npos &&
              pos_voice != std::string::npos && pos_policy < pos_core && pos_core < pos_voice,
          "module blocks in plan order");
    check(!resolution.fingerprint.empty() && resolution.fingerprint.size() == 64,
          "sha256 fingerprint");
    check(resolution.total_bytes > 0 && resolution.approx_tokens > 0 &&
              resolution.approx_tokens <= resolution.total_bytes,
          "size and token estimates");

    // Determinism: same inputs -> same fingerprint.
    const auto again = packs::resolve(manifest, sample_files(), good_plan, "test-profile", 7);
    check(again.ok && again.fingerprint == resolution.fingerprint, "fingerprint stable");

    // Content change -> different fingerprint.
    auto changed_files = sample_files();
    changed_files["test/voice.md"] = "# voice\nchanged\n";
    const auto changed = packs::resolve(manifest, changed_files, good_plan, "test-profile", 7);
    check(changed.ok && changed.fingerprint != resolution.fingerprint,
          "content change changes fingerprint");

    // Invalid plan -> clean failure, no partial prompt.
    const auto bad = packs::resolve(manifest, sample_files(), std::vector<std::string>{"test.core"},
                                    "x", 1);
    check(!bad.ok && !bad.error.empty() && bad.prompt.empty(), "invalid plan fails cleanly");

    // ── profile fingerprint includes the plan ──────────────────────────────
    section("profile fingerprint");
    {
        const std::filesystem::path db2 =
            std::filesystem::temp_directory_path() / "nyaxus_promptpacks_test2.db";
        std::filesystem::remove(db2, ec);
        SqliteStore store2(db2);
        StoredProfile a;
        a.id = "p";
        a.name = "P";
        a.prompt_pack_id = "chars";
        a.prompt_plan_json = "[\"shared.policy\",\"test.core\"]";
        a.created_at = "t";
        a.updated_at = "t";
        a.revision = 1;
        store2.upsert_profile(a);
        const auto one = store2.get_profile("p");
        a.prompt_plan_json = "[\"shared.policy\",\"test.core\",\"test.voice\"]";
        store2.upsert_profile(a);
        const auto two = store2.get_profile("p");
        check(one.has_value() && two.has_value() && one->prompt_plan_json != two->prompt_plan_json,
              "plan edit persists");
    }

    std::filesystem::remove(db_path, ec);
    if (g_fail) {
        std::cout << "\n" << g_fail << " check(s) FAILED\n";
        return 1;
    }
    std::cout << "\nall promptpacks checks passed\n";
    return 0;
}
