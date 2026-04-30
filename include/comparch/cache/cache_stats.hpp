#pragma once

#include <cstdint>

namespace comparch::cache {

struct CacheStats {
    std::uint64_t accesses          = 0;
    std::uint64_t reads             = 0;
    std::uint64_t writes            = 0;

    // WBWA accounting (project1's L1 vocabulary): every access classifies
    // as hit or miss.
    std::uint64_t hits              = 0;
    std::uint64_t misses            = 0;
    std::uint64_t writebacks        = 0;

    // WTWNA accounting (project1's L2 vocabulary): only the read path
    // counts read_hits / read_misses; writes are counted but not
    // classified as hit/miss because WTWNA never allocates on write.
    std::uint64_t read_hits         = 0;
    std::uint64_t read_misses       = 0;

    std::uint64_t prefetches_issued = 0;
    std::uint64_t prefetch_hits     = 0;
    std::uint64_t prefetch_misses   = 0;

    // Phase 5B: blocks dropped by Cache::coherence_invalidate() in
    // response to a directory-driven REQ_INVALID / RECALL_GOTO_I.
    std::uint64_t coherence_invals  = 0;
};

} // namespace comparch::cache
