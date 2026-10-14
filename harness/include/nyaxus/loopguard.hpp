#pragma once

#include <string>
#include <vector>

namespace nyaxus {
namespace loopguard {

// Design: docs/loop-guards.md. Deterministic, LLM-free guards against
// non-progress tool loops in the agent loop: the 2nd identical call runs
// with a change-approach note, the 3rd is suppressed with a
// reflect-then-pivot intervention, and a second fully-suppressed batch
// ends the run with a synthesized narrative.

// The synthetic argument key every inert shell command collapses onto. Not
// valid JSON, so it can never collide with a canonical argument blob.
constexpr const char* kInertArgs = "<inert-command>";

// Change-approach note appended to the result of a 2nd identical tool call.
constexpr const char* kRepeatNote =
    "[repeat note] This is the second time you have made this exact call "
    "(same tool, same arguments). If the previous result is not advancing "
    "you, change approach before repeating it.";

// True when `command` cannot change any state and surfaces no information
// the caller did not already have: literal echo/printf, the null builtins
// (true/false/:), and sequences of exactly those. Conservative by
// construction - substitution, redirect, pipe, subshell, or backgrounding
// anywhere in the command disqualifies it, and the quote-blind segment
// split can only ever err toward not-inert. An empty command is not inert
// (a malformed call is a different problem).
bool is_inert_command(const std::string& command);

// True when `name`/`args_json` is a terminal call whose command is inert.
// Keyed on the tool name: this is a shell-syntax judgement, and applying it
// to some future Execute tool with different argument semantics would read
// a `command` field that means something else.
bool is_inert_terminal_call(const std::string& name, const std::string& args_json);

// Canonical argument key: parse -> re-stringify. Object keys are
// std::map-ordered, so key order cannot distinguish two identical calls.
// Parse failure (or a non-object value) keeps the raw string - a malformed
// call repeated is still a repeat.
std::string canonical_args(const std::string& args_json);

struct Verdict {
    enum class Kind { Pass, Reminder, Suppress };
    Kind kind = Kind::Pass;
    bool inert = false;  // suppressed call was an inert shell command
};

// Per-run sliding-window repeat guard. The threshold-th identical
// (tool, canonical-args) call is suppressed (and not recorded, so the next
// identical call is suppressed too); the call just before it passes with a
// reminder. Mutating (non-inert) calls clear the inert entries from the
// window, so a verify-read after a real change is not flagged; inert
// commands do not clear - an echo must not launder the reads out of the
// guard's reach.
struct Guard {
    int window_size = 6;
    int threshold = 3;
    Verdict inspect(const std::string& name, const std::string& args_json);
    void reset() { recent_.clear(); }
private:
    struct Entry {
        std::string signature;
        bool mutating = true;
    };
    std::vector<Entry> recent_;
};

// Model-visible guard texts (reflect-then-pivot interventions). A bare
// "try again" reinforces the same failing chain; the guard forces the model
// to diagnose first, then diverge - a different tool, entry point, or
// assumption - with explicit permission to stop.
std::string repeat_guard_text();
std::string inert_guard_text();

// ── Runaway detector ─────────────────────────────────────────────────────
// Turn-local, pure observation over EXECUTED calls - the complement of the
// pre-dispatch Guard above: that one catches identical (tool, args) storms
// before they run; this one catches the loops that sail past it -
// near-identical polling, identical results from read-only calls the Guard
// exempts, a repeated error family, and A-B-A-B alternation. On threshold
// it emits a reminder (injected into the tool result), never a
// termination.
enum class RunawayKind { None, Polling, ResultRepeat, ErrorRepeat, Alternation };

const char* kind_name(RunawayKind kind);

// Budget for the stable result key: a 64 KiB tool output is not a
// fingerprint, so the key is lowercased, digit-masked, whitespace-
// normalized, and truncated.
constexpr std::size_t kResultKeyBudget = 1024;

struct RunawaySignal {
    RunawayKind kind = RunawayKind::None;
    std::string tool;
    std::string detail;  // ledger-facing description of the repeated shape
};

// Per-run observation window over executed (tool, args, result) triples.
// observe() records the call and returns a signal when a loop shape crosses
// the threshold. A signal consumes the streak (the window resets), so the
// same loop re-fires only after a genuinely different step breaks it - one
// reminder per crossing, not one per repeated call.
struct RunawayGuard {
    int threshold = 3;
    int window_size = 8;
    RunawaySignal observe(const std::string& tool,
                          const std::string& args_json,
                          const std::string& result);
    void reset() { recent_.clear(); }
private:
    struct Observation {
        std::string signature;  // tool + canonical args
        std::string masked;     // tool + digit-masked canonical args
        std::string result_key;  // budgeted stable result key
        std::string error_key;   // "" when the result reads as no error
    };
    std::vector<Observation> recent_;
};

// Model-visible reminder injected into the tool result when a runaway
// shape is detected: what is repeating, and the step-back (expect
// something different, or change approach, or stop and report).
std::string runaway_note(RunawayKind kind);

// Run-terminating narrative, synthesized locally (no LLM call): the run
// gave up because the model kept repeating the same dead call (or a spin
// of inert commands). House voice.
std::string give_up_text(bool inert);

}  // namespace loopguard
}  // namespace nyaxus
