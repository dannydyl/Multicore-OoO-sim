#pragma once

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "comparch/cache/cache_stats.hpp"
#include "comparch/cache/mem_req.hpp"
#include "comparch/cache/mshr.hpp"
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
        unsigned    mshr_entries  = 8;   // size of the MSHR table; bounds in-flight misses

        Cache*      next_level    = nullptr;
        MainMemory* main_memory   = nullptr;

        // Prefetcher attached to this level. Invoked after demand misses.
        std::unique_ptr<Prefetcher> prefetcher;
        // Peer cache one level up — prefetchers consult this to avoid
        // bringing blocks already resident there.
        Cache*      peer_above   = nullptr;
    };

    Cache(Config cfg, std::string name);

    // Synchronous wrapper. Mutates cache state, recurses into next_level
    // if needed, returns a complete (hit, total round-trip latency) pair.
    // Used by --mode cache and (internally) by issue().
    AccessResult access(const MemReq& req);

    // ---- Async / MSHR-aware path used by the OoO core. ---------------------
    //
    // issue(): allocate an MSHR slot for `req`. Cache state is updated
    //   immediately (the entry transitions through the same hit/miss path
    //   as access()), but the caller polls peek(id) until `ready == true`
    //   to mimic real cache latency. Returns std::nullopt if the MSHR is
    //   full — the caller (LSU / fetch) stalls.
    //
    // peek(): non-owning read of the MSHR entry behind a given id. Returns
    //   nullptr if the id is unknown (already released, or never issued).
    //
    // complete(): release the MSHR slot once the consumer has read the
    //   result.
    //
    // tick(): advance the cache's local cycle counter and flip ready bits
    //   on any MSHR entries whose due_cycle has been reached. Drives the
    //   cache forward by one cycle of simulator time.
    std::optional<std::uint64_t> issue(const MemReq& req);
    const MSHREntry*             peek(std::uint64_t id) const;
    void                         complete(std::uint64_t id);
    void                         tick();

    const CacheStats& stats() const { return stats_; }
    const std::string& name() const { return name_; }
    const Config&      cfg()  const { return cfg_; }
    std::uint64_t      now()  const { return now_; }
    const MSHR&        mshr() const { return mshr_; }

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

    // Outstanding-miss table. The OoO LSU and I-fetch poll this through
    // peek()/complete()/tick(). --mode cache never observes it (access()
    // allocates and releases an MSHR slot internally per call).
    MSHR          mshr_;
    std::uint64_t now_     = 0;   // cycle counter advanced by tick()
    std::uint64_t next_id_ = 1;   // 0 reserved as "invalid id"
};

} // namespace comparch::cache
