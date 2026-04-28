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
