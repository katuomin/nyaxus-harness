#include "nyaxus/loopguard.hpp"

#include <algorithm>
#include <cctype>

#include "nyaxus/json.hpp"

namespace nyaxus {
namespace loopguard {
namespace {

std::string trim(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

// Shell metacharacters that take a command outside "prints a literal I
// already knew": `$` and a backtick substitute in state we cannot see,
// `<`/`>` redirect, `|` pipes the output somewhere it can matter, `&`
// backgrounds, parens and braces open a subshell or group whose contents
// this scanner does not read.
bool has_outward_chars(const std::string& segment) {
    static const char kOutward[] = {'$', '`', '<', '>', '|', '&', '(', ')',
                                    '{', '}'};
    for (char c : segment) {
        for (const char outward : kOutward) {
            if (c == outward) return true;
        }
    }
    return false;
}

// True when one already-scanned statement is inert. Blank segments (a
// trailing `;` or a blank line between statements) are inert by omission.
bool is_inert_segment(const std::string& raw) {
    const std::string segment = trim(raw);
    if (segment.empty()) return true;
    if (has_outward_chars(segment)) return false;
    if (segment == "true" || segment == "false" || segment == ":") {
        return true;
    }
    // `echo` alone prints a newline; `echo <literals>` prints them back.
    // Either way the caller learns nothing, and the outward-char check
    // above has already ruled out substitutions and redirects.
    const std::size_t space = segment.find_first_of(" \t");
    const std::string verb =
        (space == std::string::npos) ? segment : segment.substr(0, space);
    return verb == "echo" || verb == "printf";
}

}  // namespace

bool is_inert_command(const std::string& command) {
    bool saw_segment = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < command.size();) {
        bool separator = false;
        std::size_t width = 1;
        if (i + 1 < command.size() &&
            ((command[i] == '&' && command[i + 1] == '&') ||
             (command[i] == '|' && command[i + 1] == '|'))) {
            separator = true;
            width = 2;
        } else if (command[i] == ';' || command[i] == '\n') {
            separator = true;
        }
        if (separator) {
            const std::string segment = command.substr(start, i - start);
            if (!is_inert_segment(segment)) return false;
            if (!trim(segment).empty()) saw_segment = true;
            start = i + width;
            i = start;
        } else {
            ++i;
        }
    }
    const std::string segment = command.substr(start);
    if (!is_inert_segment(segment)) return false;
    if (!trim(segment).empty()) saw_segment = true;
    return saw_segment;
}

bool is_inert_terminal_call(const std::string& name,
                            const std::string& args_json) {
    if (name != "terminal") return false;
    try {
        const json::Value args = json::parse(args_json);
        const std::string command = args["command"].as_string();
        return !command.empty() && is_inert_command(command);
    } catch (...) {
        return false;
    }
}

std::string canonical_args(const std::string& args_json) {
    try {
        const json::Value value = json::parse(args_json);
        if (value.type() == json::Value::Type::Object) {
            return json::stringify(value);
        }
        return args_json;
    } catch (...) {
        return args_json;
    }
}

// Digit-masked copy: volatile fields (offsets, ports, ids, timestamps) all
// collapse to '#', so a polling shape (same call, only the changing number
// differs) reads as one behavior.
std::string mask_digits(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        out.push_back(std::isdigit(static_cast<unsigned char>(c)) ? '#'
                                                                  : c);
    }
    return out;
}

// Stable result key: lowercase, whitespace runs collapsed, truncated to
// the budget. Deliberately NOT digit-masked - a progress value that CHANGES
// ("building: 40%" -> "60%") is new information, and masking the digits
// would read it as the same result. Digit-masking belongs to the error
// family key below, where a changed line number is the same failure.
std::string stable_result_key(const std::string& in) {
    std::string out;
    out.reserve(std::min(in.size(), kResultKeyBudget));
    bool last_space = false;
    for (char c : in) {
        if (out.size() >= kResultKeyBudget) break;
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isspace(uc)) {
            if (last_space) continue;
            out.push_back(' ');
            last_space = true;
        } else {
            out.push_back(static_cast<char>(std::tolower(uc)));
            last_space = false;
        }
    }
    return out;
}

// Error-family detection: tool results lead with the failure, so the
// marker search runs on the key's head. Conservative list - a result that
// merely MENTIONS an error word deep in its body is not an error repeat.
bool looks_like_error(const std::string& key_head) {
    static const char* kMarkers[] = {
        "error",    "failed",     "failure",   "exception",
        "not found", "no such",   "denied",    "refused",
        "timed out", "timeout",   "cannot",    "unable to",
    };
    for (const char* marker : kMarkers) {
        if (key_head.find(marker) != std::string::npos) return true;
    }
    return false;
}

Verdict Guard::inspect(const std::string& name,
                              const std::string& args_json) {
    const bool inert = is_inert_terminal_call(name, args_json);
    const std::string args_key = inert ? kInertArgs : canonical_args(args_json);
    const std::string signature = name + "\x00" + args_key;
    const bool mutating = !inert;
    if (mutating) {
        // Drop prior inert entries - the state just changed, so a
        // verify-read after this should start with a clean slate. Keep
        // mutating entries: three identical edits in a row is still a
        // storm.
        recent_.erase(
            std::remove_if(recent_.begin(), recent_.end(),
                           [](const Entry& entry) { return !entry.mutating; }),
            recent_.end());
    }
    int count = 0;
    for (const Entry& entry : recent_) {
        if (entry.signature == signature) ++count;
    }
    const int occurrence = count + 1;
    if (occurrence >= threshold) {
        // Suppressed calls are not recorded, so the next identical call is
        // suppressed too (the count stays at threshold - 1).
        return Verdict{Verdict::Kind::Suppress, inert};
    }
    recent_.push_back(Entry{signature, mutating});
    while (recent_.size() > static_cast<std::size_t>(window_size)) {
        recent_.erase(recent_.begin());
    }
    if (occurrence == threshold - 1) {
        return Verdict{Verdict::Kind::Reminder, false};
    }
    return Verdict{Verdict::Kind::Pass, false};
}

std::string repeat_guard_text() {
    return "[repeat-loop guard] You have made this exact call more than once "
           "and gotten the same result - you are stuck in a loop. Do NOT "
           "repeat it. Before doing anything else, work through these steps:\n"
           "1. State what you were trying to achieve with this call and why "
           "it isn't getting you there.\n"
           "2. Look at the earlier results for it above. What assumption of "
           "yours might be wrong, and what do those results actually tell "
           "you?\n"
           "3. Propose 2-3 FUNDAMENTALLY different approaches - a different "
           "tool, a different entry point, or a different interpretation of "
           "the problem - and pick the most promising one.\n"
           "4. Proceed with that approach.\n"
           "If none of them can work with the tools available, say so "
           "plainly and report what you found instead of trying again.";
}

std::string inert_guard_text() {
    return "[inert-loop guard] Your last several shell commands did nothing "
           "- they changed no state and told you nothing you didn't already "
           "know (echo, true, and the like). You are spinning. Do NOT run "
           "another one. Before doing anything else, work through these "
           "steps:\n"
           "1. State plainly what you are actually trying to do right now.\n"
           "2. If it needs a tool other than the terminal, call that tool "
           "directly - a shell command cannot invoke another tool, and "
           "narrating that you intend to call one does not call it. Check "
           "the tool list you were given for its exact name and argument "
           "schema.\n"
           "3. If the tool you want does not exist, say so plainly and "
           "either do the job with the tools you have or stop and report "
           "what is blocking you.\n"
           "4. Proceed. Your next action must be either a real tool call "
           "that does work or a message to the user - not another "
           "placeholder command.";
}

const char* kind_name(RunawayKind kind) {
    switch (kind) {
        case RunawayKind::Polling: return "polling";
        case RunawayKind::ResultRepeat: return "result-repeat";
        case RunawayKind::ErrorRepeat: return "error-repeat";
        case RunawayKind::Alternation: return "alternation";
        default: return "none";
    }
}

RunawaySignal RunawayGuard::observe(const std::string& tool,
                                    const std::string& args_json,
                                    const std::string& result) {
    const std::string canonical = canonical_args(args_json);
    const std::string signature = tool + "\x00" + canonical;
    const std::string masked = tool + "\x00" + mask_digits(canonical);
    const std::string result_key = stable_result_key(result);
    // Error-family key: the digit-masked head, so the same failure with a
    // changed line number/offset reads as the same family.
    const std::string key_head =
        mask_digits(stable_result_key(result).substr(0, 512));
    const std::string error_key = looks_like_error(key_head) ? key_head : "";

    const std::size_t n = recent_.size();
    const std::size_t need = static_cast<std::size_t>(threshold - 1);
    if (n >= need) {
        // A-B-A-B: the previous three plus this one strictly alternate
        // between two distinct signatures. Signature-only on purpose - the
        // pre-dispatch Guard already owns identical-signature storms, so an
        // alternation is by definition between calls it let through.
        if (n >= 3) {
            const std::string& a =
                recent_[n - 3].signature;
            const std::string& b =
                recent_[n - 2].signature;
            const std::string& c =
                recent_[n - 1].signature;
            if (a == c && b != a && signature == b) {
                recent_.clear();  // consume the crossing
                return {RunawayKind::Alternation, tool,
                        "alternating between two calls"};
            }
        }
        // Identical-result repeat: the previous (threshold-1) calls share
        // this exact signature AND result key. For calls the pre-dispatch
        // Guard tracks, the 2nd result carries the repeat note (different
        // key) and the 3rd is suppressed, so this fires on the exempt
        // read-only tools - a recall/search poll that never changes.
        bool same_signature_result = !result_key.empty();
        bool same_result_key = !result_key.empty();
        bool same_masked = true;
        bool varied_signature = false;
        bool same_error = !error_key.empty();
        bool same_tool = true;
        for (std::size_t i = n - need; i < n; ++i) {
            if (recent_[i].signature != signature) {
                same_signature_result = false;
                varied_signature = true;
            }
            if (recent_[i].result_key != result_key) {
                same_signature_result = false;
                same_result_key = false;
            }
            if (recent_[i].masked != masked) same_masked = false;
            if (recent_[i].error_key != error_key) same_error = false;
            if (recent_[i].signature.compare(0, tool.size() + 1, signature, 0,
                                             tool.size() + 1) != 0) {
                same_tool = false;
            }
        }
        if (same_signature_result) {
            recent_.clear();  // consume the crossing
            return {RunawayKind::ResultRepeat, tool,
                    "identical call, identical result"};
        }
        // Polling: same tool, digit-masked-identical arguments (the volatile
        // fields changed), same result. A genuinely changing result is
        // progress - the streak is not a loop. (Equal signatures with equal
        // results already returned as ResultRepeat above.)
        if (same_result_key && same_masked && varied_signature) {
            recent_.clear();
            return {RunawayKind::Polling, tool,
                    "near-identical calls with the same result"};
        }
        // Same-error-family repeat: the same tool keeps failing with the
        // same normalized error, arguments or not.
        if (same_tool && same_error) {
            recent_.clear();
            return {RunawayKind::ErrorRepeat, tool,
                    "the same error repeated"};
        }
    }

    recent_.push_back({signature, masked, result_key, error_key});
    while (recent_.size() > static_cast<std::size_t>(window_size)) {
        recent_.erase(recent_.begin());
    }
    return RunawaySignal{};
}

std::string runaway_note(RunawayKind kind) {
    std::string head;
    switch (kind) {
        case RunawayKind::Polling:
            head = "You are repeating a near-identical call (only the "
                   "volatile details change) and getting the same result "
                   "back.";
            break;
        case RunawayKind::ResultRepeat:
            head = "You are repeating the same call and getting the same "
                   "result back.";
            break;
        case RunawayKind::ErrorRepeat:
            head = "The same error keeps coming back from your last several "
                   "calls.";
            break;
        case RunawayKind::Alternation:
            head = "You are ping-ponging between two calls without making "
                   "progress.";
            break;
        default:
            break;
    }
    return "[loop reminder] " + head +
           " Repeating it is not getting you new information. Before your "
           "next call: state what you expect to be different this time, and "
           "if nothing is, change approach - a different tool, a different "
           "entry point, or stop and report what you have.";
}

std::string give_up_text(bool inert) {
    if (inert) {
        return "i've stopped here because i was spinning. i kept running "
               "shell commands that did nothing - echo, true, and the like - "
               "instead of taking a real next step, and running more of them "
               "wasn't going to get me any further.\n\n"
               "i wasn't able to finish what you asked. if you can confirm "
               "the goal or point me at the next concrete step, i'll pick it "
               "back up from there.";
    }
    return "i've stopped here to avoid spinning in a loop. i kept making "
           "the same tool call and getting the same result, and repeating it "
           "wasn't going to get me any further, so i'd rather pause than "
           "burn the session retrying a dead end.\n\n"
           "i wasn't able to finish what you asked. if you can confirm the "
           "goal, point me at the right file, or suggest a different angle, "
           "i'll pick it back up from there.";
}

}  // namespace loopguard
}  // namespace nyaxus
