#pragma once

#include "comparch/cache/prefetcher.hpp"

namespace comparch::cache {

// +1 prefetcher: on a demand miss for block B, issue a prefetch for B+1
// only if neither L1 nor L2 already holds it. Mirror of project1's
// plus_one_prefetcher.
class PlusOnePrefetcher : public Prefetcher {
public:
    void on_miss(Cache& cache, Cache* peer, const MemReq& demand) override;
};

} // namespace comparch::cache
