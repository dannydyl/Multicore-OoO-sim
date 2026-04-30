#pragma once

// Per-cache sink invoked when a miss or eviction can't be satisfied by
// next_level / main_memory and a coherence protocol must take over. The
// concrete implementation lives in the coherence module
// (`coherence::CoherenceAdapter`); this header stays in the cache module
// to avoid pulling coherence headers into every Cache consumer.
//
// Phase 5B contract:
//   on_miss   — called from Cache::access() / Cache::issue() on a tag
//               miss when coherence_sink is wired. The sink is
//               responsible for translating the miss into a network
//               message, issuing it, and eventually filling the cache
//               (via Cache::insert_new_block) and flipping the MSHR
//               slot's ready flag (via Cache::mark_ready).
//
//   on_evict  — called from Cache::insert_new_block() when a victim
//               block is evicted. The sink decides whether to send a
//               WRITEBACK (dirty) or INVACK (clean) message to the
//               directory based on the block's coherence state.

#include <climits>
#include <cstdint>

#include "comparch/cache/mem_req.hpp"

namespace comparch::cache {

// Sentinel returned in AccessResult::latency when the miss has been
// routed to a CoherenceSink and will complete asynchronously via
// Cache::mark_ready. Cache::issue detects this and sets the MSHR
// slot's due_cycle to UINT64_MAX so the per-tick auto-ready logic
// never flips the bit on its own.
inline constexpr unsigned int kCoherenceSuspendedLatency = UINT_MAX;

class CoherenceSink {
public:
    virtual ~CoherenceSink() = default;
    virtual void on_miss(std::uint64_t block_addr, Op op) = 0;
    virtual void on_evict(std::uint64_t block_addr, bool dirty) = 0;
};

} // namespace comparch::cache
