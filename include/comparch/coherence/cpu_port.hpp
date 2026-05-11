#pragma once

// Abstract CPU-side port that a coherence::Cache can write a `DATA`
// response into. FiciCpu (used by --mode coherence) and
// CoherenceAdapter (used by full mode) both implement this so the
// agent's send_DATA_proc helper stays protocol-agnostic.

#include "comparch/coherence/message.hpp"

namespace comparch::coherence {

class CpuPort {
public:
    virtual ~CpuPort() = default;

    // Per-cycle entry points called by Node::tick / Node::tock.
    virtual void tick()        = 0;
    virtual void tock()        = 0;
    virtual bool is_done() const = 0;

    // Cache hook: called after coherence::Cache::tick has dispatched
    // the agent for a network-side message. Lets the adapter react to
    // invalidations / recalls by dropping resident lines from the
    // underlying finite cache. FiciCpu leaves it as a no-op.
    virtual void on_ntwk_event(const Message& /*req*/) {}

    // Mailbox where Cache::tick / Agent::send_DATA_proc deposit the
    // returned DATA message. The owner reads this slot and clears it.
    Message* cache_in_next = nullptr;
};

} // namespace comparch::coherence
