#pragma once

// Abstract CPU-side port that a coherence::Cache can write a `DATA`
// response into. Phase 5A's FiciCpu and Phase 5B's CoherenceAdapter
// both implement this interface so the coherence agent's send_DATA_proc
// helper stays protocol-agnostic.

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
    // underlying finite cache. FiciCpu (5A) leaves it as a no-op.
    virtual void on_ntwk_event(const Message& /*req*/) {}

    // Mailbox where Cache::tick / Agent::send_DATA_proc deposit the
    // returned DATA message. The owner reads this slot and clears it.
    Message* cache_in_next = nullptr;
};

} // namespace comparch::coherence
