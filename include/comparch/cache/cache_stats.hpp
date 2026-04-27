#pragma once

#include <cstdint>

namespace comparch::cache {

struct CacheStats {
    std::uint64_t accesses          = 0;
    std::uint64_t reads             = 0;
    std::uint64_t writes            = 0;
    std::uint64_t hits              = 0;
    std::uint64_t misses            = 0;
    std::uint64_t writebacks        = 0;

    std::uint64_t prefetches_issued = 0;
    std::uint64_t prefetch_hits     = 0;
    std::uint64_t prefetch_misses   = 0;
};

} // namespace comparch::cache
