#pragma once

#include "comparch/cache/mem_req.hpp"

namespace comparch::cache {

class Cache;

// Abstract prefetcher attached to a cache level. on_miss is called after
// the demand block has been allocated in `cache`; `peer` (when non-null)
// is the level above so prefetchers can avoid bringing blocks already
// resident there. Prefetchers issue requests via cache.issue_prefetch().
class Prefetcher {
public:
    virtual ~Prefetcher() = default;

    virtual void on_miss(Cache& cache, Cache* peer, const MemReq& demand) = 0;
};

} // namespace comparch::cache
