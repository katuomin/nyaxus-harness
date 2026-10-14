#include "nyaxus/streamrules.hpp"

#include <tuple>

namespace nyaxus {
namespace streamrules {
namespace {

// The built-in rule set. Both rules fire only while NO tool has run in the
// run: once a tool has executed, a claim or a narrated intent is grounded in
// real work and the rule goes quiet. Deliberately conservative - a stream
// rule tax costs every run it fires in, and a false positive trains the
// model to distrust its own completions.

std::vector<std::tuple<std::string, std::regex, std::string>> builtin_rules() {
    return {
        {"work-claim",
         std::regex("\\b(done|finished|completed|fixed|resolved|shipped)\\b",
                    std::regex::icase),
         "[stream rule] You wrote that the work is done, but no tool has run "
         "in this turn yet. Back a completion claim with a tool result before "
         "you make it - or say plainly what you have not checked."},
        {"narrated-intent",
         // "let me know" is a courtesy, not an intent to act - excluded
         // with a lookahead.
         std::regex(
             "\\b(i will now|let me(?! know)|i'?ll now|next,? i will|"
             "i'?m going to|going to now)\\b",
             std::regex::icase),
         "[stream rule] You are narrating your next action. Take it - the "
         "tool call is the action. If no tool can do it, say so in your "
         "answer instead of describing it."},
    };
}

}  // namespace

Rules::Rules() {
    for (const auto& [id, pattern, reminder] : builtin_rules()) {
        rules_.push_back(Rule{id, pattern, reminder, false});
    }
}

std::vector<Firing> Rules::observe(const std::string& text,
                                   bool tools_ran) {
    std::vector<Firing> fired;
    if (tools_ran || text.empty()) return fired;
    for (Rule& rule : rules_) {
        if (rule.fired) continue;
        std::smatch match;
        if (std::regex_search(text, match, rule.pattern)) {
            rule.fired = true;
            fired.push_back(Firing{rule.id, rule.reminder});
        }
    }
    return fired;
}

void Rules::reset() {
    for (Rule& rule : rules_) rule.fired = false;
}

}  // namespace streamrules
}  // namespace nyaxus
