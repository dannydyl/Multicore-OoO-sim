#include "comparch/cache/main_memory.hpp"

namespace comparch::cache {

MainMemory::MainMemory(Config cfg) : cfg_(cfg) {}

AccessResult MainMemory::access(const MemReq& req) {
    ++stats_.accesses;
    ++stats_.hits;
    if (req.op == Op::Write)     ++stats_.writes;
    else                          ++stats_.reads;
    AccessResult r;
    r.hit     = true;
    r.latency = cfg_.latency;
    return r;
}

} // namespace comparch::cache
