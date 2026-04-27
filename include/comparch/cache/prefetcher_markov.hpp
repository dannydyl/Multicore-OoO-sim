#pragma once

#include <cstdint>
#include <list>

#include "comparch/cache/prefetcher.hpp"

namespace comparch::cache {

// Markov prefetcher: per-source-block table of (destination_b_addr,
// num_access) entries. On a demand miss, look up the missed block in the
// table and prefetch its MFU successor (if any). Mirror of project1's
// markov_prefetecher.
class MarkovPrefetcher : public Prefetcher {
public:
    explicit MarkovPrefetcher(unsigned max_rows);

    void on_miss(Cache& cache, Cache* peer, const MemReq& demand) override;

    // Exposed for HybridPrefetcher reuse.
    struct markov_t {
        std::uint64_t destination_b_addr = 0;
        std::uint64_t num_access         = 0;
    };

    struct markov_row {
        std::uint64_t        source_b_addr = 0;
        std::list<markov_t>  MKV_row;
    };

protected:
    std::list<markov_row> MKV_col;
    std::uint64_t prev_block      = 0;
    bool          pf_first_block  = true;
    unsigned      MAX_MKV_rows    = 0;

    static void          markov_row_sort(std::list<markov_t>& row);
    static std::uint64_t get_MFU_block_addr(std::list<markov_t>& row);
    static bool          is_dest_in_MRK_row(std::list<markov_t>& row,
                                            std::uint64_t missed_addr);
    void                 add_new_entry(std::list<markov_t>& row,
                                       std::uint64_t missed_addr);
    void                 add_new_row(std::uint64_t missed_addr);

    // Common table-update logic shared with the hybrid prefetcher.
    void learn(std::uint64_t missed_block_addr);
};

} // namespace comparch::cache
