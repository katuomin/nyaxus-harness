#pragma once

#include <string>
#include <vector>

namespace nyaxus {
namespace commandplane {

// The durable command plane: Run / Steer / Interrupt /
// RespondInput as append-only, immutable entries. The host (the daemon)
// is the sole writer of outcomes; clients only submit entries.
enum class Kind { Run, Steer, Interrupt, RespondInput };

// The verdict the host applies to one entry.
enum class Verdict {
    Execute,     // apply the command
    Skip,        // already processed (idempotent re-delivery)
    Expired,     // past its TTL
    Superseded,  // a newer command replaced it
};

struct Command {
    std::string id;             // client-generated; the dedupe key
    Kind kind = Kind::Steer;
    long long issued_at_ms = 0; // wall-clock, for TTL
    long long ttl_ms = 0;       // 0 = no TTL
    // The turn index an interrupt targets; -1 = the current turn. A
    // target at or past the live turn is already done -> Superseded.
    long long target_turn = -1;
};

struct State {
    long long now_ms = 0;
    long long current_turn = 0;  // the run's live turn index
    // Host-owned record of ids whose outcome was written (the only writer
    // is the host, so this set is authoritative for dedupe).
    std::vector<std::string> processed_ids;
};

// Pure drain evaluation:
//   1. processed-id dedupe -> Skip (idempotent re-delivery, restart replay)
//   2. TTL expiry -> Expired
//   3. Interrupt: a newer interrupt for the same target in `pending`
//      -> Superseded; a target turn that has already passed -> Superseded
//   4. else Execute
// No I/O, no globals: the same inputs always yield the same verdict, which
// is what makes the queue safe to replay after a restart.
Verdict evaluate(const Command& command, const State& state,
                 const std::vector<Command>& pending = {});

const char* kind_name(Kind kind);
const char* verdict_name(Verdict verdict);

}  // namespace commandplane
}  // namespace nyaxus
