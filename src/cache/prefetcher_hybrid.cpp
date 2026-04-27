#include "comparch/cache/prefetcher_hybrid.hpp"

#include "comparch/cache/cache.hpp"

namespace comparch::cache {

HybridPrefetcher::HybridPrefetcher(unsigned max_rows)
    : MarkovPrefetcher(max_rows) {}

// Mirror of project1's hybrid_prefetcher.
void HybridPrefetcher::on_miss(Cache& cache, Cache* peer,
                               const MemReq& demand) {
    const std::uint64_t missed_block_addr = cache.get_block_addr(demand.addr);

    if (MAX_MKV_rows == 0) {
        plus_one_.on_miss(cache, peer, demand);
        pf_first_block = false;
        prev_block     = missed_block_addr;
        return;
    }

    // ---- check whether Markov has the row ----
    bool found_row      = false;
    bool has_prediction = false;
    std::uint64_t MFU_block = 0;

    for (auto row = MKV_col.begin(); row != MKV_col.end(); ++row) {
        if (row->source_b_addr == missed_block_addr) {
            found_row = true;
            if (!row->MKV_row.empty()) {
                has_prediction = true;
                MFU_block      = get_MFU_block_addr(row->MKV_row);
            }
            break;
        }
    }

    if (has_prediction) {
        const std::uint64_t MFU_byte = MFU_block << cache.cfg().b;
        const bool in_peer = peer ? peer->block_in(MFU_byte) : false;
        const bool in_self = cache.block_in(MFU_byte);
        if (!in_peer && !in_self && MFU_block != missed_block_addr) {
            cache.issue_prefetch(MFU_byte);
        }
    } else if (!found_row) {
        plus_one_.on_miss(cache, peer, demand);
    }

    if (pf_first_block) {
        pf_first_block = false;
        prev_block     = missed_block_addr;
        return;
    }

    learn(missed_block_addr);

    prev_block = missed_block_addr;
}

} // namespace comparch::cache
