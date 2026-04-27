#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "comparch/cache/cache_stats.hpp"
#include "comparch/cache/mem_req.hpp"
#include "comparch/cache/prefetcher.hpp"
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

        // Prefetcher attached to this level. Invoked after demand misses.
        std::unique_ptr<Prefetcher> prefetcher;
        // Peer cache one level up — prefetchers consult this to avoid
        // bringing blocks already resident there.
        Cache*      peer_above   = nullptr;
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

    // True iff the block containing `byte_addr` is currently resident.
    bool          block_in(std::uint64_t byte_addr) const;

    // Prefetch helper: if not already resident, allocate the block tagged
    // as a prefetched fill and bump the issued-prefetch counter.
    void          issue_prefetch(std::uint64_t byte_addr);

    // Patch the upstream peer pointer post-construction (resolves the
    // L1<->L2 circular reference that prefetchers consult).
    void          set_peer_above(Cache* peer) { cfg_.peer_above = peer; }

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
