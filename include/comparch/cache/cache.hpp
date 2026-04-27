#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "comparch/cache/cache_stats.hpp"
#include "comparch/cache/mem_req.hpp"
#include "comparch/cache/replacement.hpp"
#include "comparch/cache/write_policy.hpp"

namespace comparch::cache {

class MainMemory;

// Block metadata. Carries the same fields as project1's tag_meta_t plus the
// prefetched flag used by the L2-side prefetchers.
struct tag_meta_t {
    std::uint64_t block_addr  = 0;
    std::uint64_t tag         = 0;
    bool          dirty       = false;
    bool          valid       = false;
    bool          prefetched  = false;
};

struct set_t {
    std::list<tag_meta_t> LRU_list; // MRU at head, LRU at tail
};

class Cache {
public:
    struct Config {
        // Geometry: matches project1's (C, B, S) — log2 capacity, log2 block,
        // log2 ways.
        unsigned    c = 10;
        unsigned    b = 6;
        unsigned    s = 1;

        Replacement replacement   = Replacement::LRU_MIP;
        WritePolicy write_policy  = WritePolicy::WBWA;
        unsigned    hit_latency   = 2;

        Cache*      next_level    = nullptr;
        MainMemory* main_memory   = nullptr;
    };

    Cache(Config cfg, std::string name);

    AccessResult access(const MemReq& req);

    const CacheStats& stats() const { return stats_; }
    const std::string& name() const { return name_; }
    const Config&      cfg()  const { return cfg_; }

    // Internals exposed for prefetchers and L2 writebacks (later phases).
    bool          is_in_cache(std::uint64_t tag, std::uint64_t index) const;
    void          insert_new_block(char rw, std::uint64_t tag,
                                   std::uint64_t index,
                                   std::uint64_t block_addr,
                                   bool is_prefetch);

    std::uint64_t get_tag(std::uint64_t block_addr) const;
    std::uint64_t get_index(std::uint64_t block_addr) const;
    std::uint64_t get_block_addr(std::uint64_t addr) const;

private:
    Config       cfg_;
    std::string  name_;
    CacheStats   stats_;

    // Geometry derived from (C, B, S) — same names as project1.
    std::uint64_t assoc      = 0;
    std::uint64_t num_rows   = 0;
    std::uint64_t index_bit  = 0;

    std::vector<set_t> rows; // was L1_row / L2_row in project1
};

} // namespace comparch::cache
