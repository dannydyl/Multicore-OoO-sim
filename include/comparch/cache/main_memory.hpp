#pragma once

#include "comparch/cache/cache_stats.hpp"
#include "comparch/cache/mem_req.hpp"

namespace comparch::cache {

class MainMemory {
public:
    struct Config {
        unsigned int latency = 100;
    };

    explicit MainMemory(Config cfg);

    AccessResult access(const MemReq& req);

    const CacheStats& stats() const { return stats_; }

private:
    Config     cfg_;
    CacheStats stats_;
};

} // namespace comparch::cache
