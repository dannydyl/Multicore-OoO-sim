#pragma once

// LlsCache --- last-level-shared cache, owned by the directory in
// shared_lls mode. Models capacity + associativity + LRU; does NOT model
// MSHRs, prefetchers, write policies, or any of the cache::Cache
// machinery. The directory consults this cache on every coherence
// miss to decide whether DATA can come from on-chip (LLS hit, latency =
// settings.lls_hit_latency) or must go off-chip (LLS miss, latency =
// settings.mem_latency, plus an install side-effect).
//
// We don't reuse cache::Cache here because the role is different:
// cache::Cache is on the per-cycle pipeline path (issue/peek MSHR-aware,
// async, prefetched, write-policy-aware). The LLS just answers
// "is this block on-chip?" and tracks LRU + evictions. A purpose-built
// class is ~150 lines vs. wrapping cache::Cache's ~600.
//
// Phase 2 only. Snoop-filter side (asking the LLS "would any L1 hit
// on a snoop?") layers on top in Phase 3.

#include <cstdint>
#include <list>
#include <vector>

#include "comparch/coherence/types.hpp"

namespace comparch::coherence {

// Result of an LlsCache::access(). Hit/miss + the block id of the
// victim, if installing this block evicted one. The directory uses the
// victim id to back-invalidate sharers under inclusive policy.
struct LlsLookup {
    bool             hit         = false;
    bool             evicted     = false;     // true iff a clean victim was kicked out
    BlockId          victim      = 0;         // valid only when evicted == true
};

class LlsCache {
public:
    // size_blocks must be divisible by assoc. Construction throws
    // ConfigError otherwise. assoc=0 or size_blocks=0 disables the
    // cache (every access is a miss with no install) — used to model
    // "no LLS" inside the directory's miss path without branching.
    LlsCache(std::size_t size_blocks, std::size_t assoc);

    // Probe-only lookup: returns hit/miss but does NOT install.
    // No LRU update either. Use this when the directory wants to
    // know whether the line is on-chip without committing to a fill.
    bool contains(BlockId block) const;

    // Install (or refresh) `block`. Returns lookup result with hit=true
    // if the block was already resident (LRU updated, no eviction);
    // otherwise hit=false, and `evicted` indicates whether a victim
    // was kicked out to make room.
    LlsLookup access(BlockId block);

    // Drop a block from the cache. No-op if the block isn't present.
    // Used when the protocol logically removes a line (e.g. all
    // sharers have invalidated and the line state collapses to I).
    void invalidate(BlockId block);

    std::size_t size_blocks() const { return size_blocks_; }
    std::size_t assoc()       const { return assoc_; }
    std::size_t num_sets()    const { return num_sets_; }

private:
    // Set index from BlockId. The block_size is folded into the BlockId
    // by the caller (BlockId is a block-aligned address >> block_log2),
    // so we just modulo by the set count.
    std::size_t set_of(BlockId block) const {
        return static_cast<std::size_t>(block) % num_sets_;
    }

    std::size_t              size_blocks_;
    std::size_t              assoc_;
    std::size_t              num_sets_;
    // sets_[i] is an MRU-front, LRU-tail list of resident block ids.
    // BlockId 0 is a legitimate id, so we use a sentinel-free
    // representation and rely on list size to bound capacity.
    std::vector<std::list<BlockId>> sets_;
};

} // namespace comparch::coherence
