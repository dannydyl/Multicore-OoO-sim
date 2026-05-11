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

    // Software thread that issued this request. Source of truth is
    // OooCore::active_tid_; the core stamps this field at issue time
    // and the cache cascade (writebacks, fills, prefetches) propagates
    // it unchanged. Defaults to 0 so single-thread paths and existing
    // brace-init call sites that omit it read as "thread 0", matching
    // pre-multithread behavior.
    //
    // Currently dropped at the coherence_sink boundary (the directory
    // still tracks sharers by NodeId == core ID, since with 1:1
    // thread-to-core mapping TID == core ID). When ThreadScheduler
    // lands and a thread can move between cores, this field becomes
    // load-bearing for per-thread coherence stats and for routing
    // SyncCoordinator notifications back to the right thread.
    std::uint32_t tid            = 0;
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
