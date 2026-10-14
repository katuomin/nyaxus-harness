#include "nyaxus/commandplane.hpp"

namespace nyaxus {
namespace commandplane {

const char* kind_name(Kind kind) {
    switch (kind) {
        case Kind::Run: return "run";
        case Kind::Steer: return "steer";
        case Kind::Interrupt: return "interrupt";
        case Kind::RespondInput: return "respond_input";
    }
    return "unknown";
}

const char* verdict_name(Verdict verdict) {
    switch (verdict) {
        case Verdict::Execute: return "execute";
        case Verdict::Skip: return "skip";
        case Verdict::Expired: return "expired";
        case Verdict::Superseded: return "superseded";
    }
    return "unknown";
}

namespace {

bool contains(const std::vector<std::string>& values,
              const std::string& value) {
    for (const std::string& v : values) {
        if (v == value) return true;
    }
    return false;
}

// A pending interrupt supersedes `command` when it targets the same turn
// and is newer (later issue time; the id breaks exact ties so two
// identical redeliveries never supersede each other).
bool superseded_by(const Command& command,
                   const std::vector<Command>& pending) {
    for (const Command& other : pending) {
        if (other.kind != Kind::Interrupt || other.id == command.id) continue;
        if (other.target_turn != command.target_turn) continue;
        if (other.issued_at_ms > command.issued_at_ms ||
            (other.issued_at_ms == command.issued_at_ms &&
             other.id > command.id)) {
            return true;
        }
    }
    return false;
}

}  // namespace

Verdict evaluate(const Command& command, const State& state,
                 const std::vector<Command>& pending) {
    // 1. Idempotency first: an outcome already written for this id means
    //    the entry is a re-delivery (or a restart replay), not new work.
    if (!command.id.empty() && contains(state.processed_ids, command.id)) {
        return Verdict::Skip;
    }
    // 2. TTL: an entry older than its time-to-live is stale - the state it
    //    was issued against is long gone, so executing it would be a guess.
    if (command.ttl_ms > 0 &&
        state.now_ms - command.issued_at_ms > command.ttl_ms) {
        return Verdict::Expired;
    }
    if (command.kind == Kind::Interrupt) {
        // 3a. A newer interrupt for the same target wins; the older one is
        //     superseded (its effect - stopping that turn - is already
        //     requested by the newer one).
        if (superseded_by(command, pending)) {
            return Verdict::Superseded;
        }
        // 3b. An interrupt aimed at a turn that has already passed (or the
        //     current one, which is being served by the live run's own
        //     cancel) is moot.
        if (command.target_turn >= 0 &&
            command.target_turn < state.current_turn) {
            return Verdict::Superseded;
        }
    }
    return Verdict::Execute;
}

}  // namespace commandplane
}  // namespace nyaxus
