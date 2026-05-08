#pragma once

#include <cstdint>

namespace comparch::cache {

enum class Op {
    Read,
    Write,
    Prefetch,
};

struct MemReq {
    std::uint64_t addr = 0;
    Op            op   = Op::Read;
    std::uint64_t pc   = 0;

    // The op the topmost cache level was asked to perform. L1 sets this
    // to its own req.op when forwarding a miss down to L2, so L2 can
    // tell coherence_sink->on_miss whether the line is destined to be
    // dirtied (write-miss → fill L1 dirty). Defaults to Read so paths
    // that never set it (prefetches, write-back traffic) behave as
    // before.
    Op            originating_op = Op::Read;
};

// Synchronous result of a Cache::access() call: was it a hit, and what is
// the total round-trip latency (including any downstream cascade). The
// async OoO path uses Cache::issue() / peek() / complete() with an
// MSHREntry instead — see comparch/cache/mshr.hpp.
struct AccessResult {
    bool         hit     = false;
    unsigned int latency = 0;
};

inline char rw_of(Op op) {
    return (op == Op::Write) ? 'W' : 'R';
}

} // namespace comparch::cache
