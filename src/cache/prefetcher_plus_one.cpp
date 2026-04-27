#include "comparch/cache/prefetcher_plus_one.hpp"

#include "comparch/cache/cache.hpp"

namespace comparch::cache {

void PlusOnePrefetcher::on_miss(Cache& cache, Cache* peer,
                                const MemReq& demand) {
    // Mirror of project1's plus_one_prefetcher.
    const std::uint64_t missed_block_addr = cache.get_block_addr(demand.addr);
    const std::uint64_t prefetch_block    = missed_block_addr + 1;
    const std::uint64_t prefetch_byte_addr = prefetch_block << cache.cfg().b;

    const bool in_peer = peer ? peer->block_in(prefetch_byte_addr) : false;
    const bool in_self = cache.block_in(prefetch_byte_addr);
    if (in_peer || in_self) {
        return;
    }
    cache.issue_prefetch(prefetch_byte_addr);
}

} // namespace comparch::cache
