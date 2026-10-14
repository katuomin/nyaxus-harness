#pragma once

#include <regex>
#include <string>
#include <vector>

namespace nyaxus {
namespace streamrules {

// Stream rules: dormant regex rules over the assistant text streamed within
// a run. A rule that fires injects one reminder line into the prompt-pack
// volatile tail for the rest of the run - no model round-trip, once per rule
// per run (deduped). The reminder is volatile: it dies with the run, so the
// fix sticks, not the tax. Pairs with the loop-guard vocabulary (the
// in-stream pre-close variant).

// One fired rule: the rule id (ledger) and the model-visible reminder
// (volatile tail + ledger text).
struct Firing {
    std::string id;
    std::string reminder;
};

// Per-run rule state. observe() takes the current assistant message's
// accumulated text and whether any tool has run in this run so far; it
// returns the rules that newly fired (empty when none). A rule fires at
// most once until reset() - one reminder per rule per run.
class Rules {
public:
    Rules();
    std::vector<Firing> observe(const std::string& text, bool tools_ran);
    void reset();

private:
    struct Rule {
        std::string id;
        std::regex pattern;
        std::string reminder;
        bool fired = false;
    };
    std::vector<Rule> rules_;
};

}  // namespace streamrules
}  // namespace nyaxus
