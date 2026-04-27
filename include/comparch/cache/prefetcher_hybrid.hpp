#pragma once

#include "comparch/cache/prefetcher_markov.hpp"
#include "comparch/cache/prefetcher_plus_one.hpp"

namespace comparch::cache {

// Hybrid prefetcher: prefer Markov when the table has a prediction for the
// missed source; fall back to +1 when the missed block has no row at all.
// Mirror of project1's hybrid_prefetcher.
class HybridPrefetcher : public MarkovPrefetcher {
public:
    explicit HybridPrefetcher(unsigned max_rows);

    void on_miss(Cache& cache, Cache* peer, const MemReq& demand) override;

private:
    PlusOnePrefetcher plus_one_;
};

} // namespace comparch::cache
