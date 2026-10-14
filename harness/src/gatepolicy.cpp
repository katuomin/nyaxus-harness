#include "nyaxus/gatepolicy.hpp"

namespace nyaxus {
namespace gatepolicy {
namespace {

CallRisk low() {
    return {"low", "none"};
}

CallRisk workspace() {
    return {"medium", "workspace"};
}

CallRisk system() {
    return {"system", "system"};
}

}  // namespace

CallRisk classify(const std::string& tool, const std::string& args_json,
                  bool read_only_call, bool posix_shell) {
    (void)args_json;
    if (read_only_call) return low();
    // Harness-state edits: the prompt pack is the harness's own memory.
    if (tool == "prompt.edit") return system();
    if (tool == "terminal") {
        // A unix /bin/sh write is scoped to the workspace directory (the
        // daemon cd's into it before every call); the cmd.exe fallback is
        // not provably scoped, so it classifies as system.
        return posix_shell ? workspace() : system();
    }
    // Everything else that writes (memory.save, web.use, ...) touches
    // harness/workspace state.
    return workspace();
}

}  // namespace gatepolicy
}  // namespace nyaxus
