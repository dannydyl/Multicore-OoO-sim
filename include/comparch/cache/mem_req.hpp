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

struct AccessResult {
    bool         hit     = false;
    unsigned int latency = 0;
};

inline char rw_of(Op op) {
    return (op == Op::Write) ? 'W' : 'R';
}

} // namespace comparch::cache
