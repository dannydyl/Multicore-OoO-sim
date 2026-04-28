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
};

// PHASE4 limit: a synchronous (hit, latency) pair is enough for trace-driven
// --mode cache where one access is one transaction, but the OoO core will
// need a request handle (or callback) so multiple in-flight misses can be
// tracked concurrently — i.e. real MSHRs. When that lands, AccessResult will
// either grow a std::shared_future-style handle or this struct will be
// replaced by an async API. Don't add new code that assumes synchronous
// completion.
struct AccessResult {
    bool         hit     = false;
    unsigned int latency = 0;
};

inline char rw_of(Op op) {
    return (op == Op::Write) ? 'W' : 'R';
}

} // namespace comparch::cache
