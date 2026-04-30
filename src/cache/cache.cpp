// Cache
// =====
// One generic cache level. Same class is used for both L1 and L2 — the
// configuration (geometry, write policy, replacement, prefetcher,
// downstream pointer) decides the behavior.
//
// Address layout (parameterized by C, B, S — log2 capacity, log2 block,
// log2 ways):
//
//   |<------------- byte address (64 bits) ----------------->|
//   |        tag        |    index    |     block offset     |
//                       ^-- index_bit  ^-- B bits
//                       = C - B - S
//
//   - block offset (B bits): byte position within a cache line (ignored
//     once we drop these bits).
//   - index (C - B - S bits): which set this address maps to.
//   - tag (everything above): identifies a unique block within a set.
//
//   Number of sets   = 2^(C - B - S)
//   Ways (assoc)     = 2^S
//   Total capacity   = 2^C bytes
//
// Each set is an ordered list (MRU at front, LRU at back) of up to `assoc`
// resident blocks. A list (instead of an array) makes it cheap to splice
// blocks to MRU on hit and pop the LRU on eviction.
//
// The structure here is a faithful port of project1's cachesim.cpp; the
// globals there became class members and the per-level entry points
// (sim_access / write_op_L2 / insert_new_block_to_L1) collapsed into the
// single Cache::access plus Cache::insert_new_block, which dispatch on
// the configured write_policy.

#include "comparch/cache/cache.hpp"

#include <stdexcept>
#include <utility>

#include "comparch/cache/main_memory.hpp"
#include "comparch/cache/prefetcher.hpp"

namespace comparch::cache {

// Construct a cache from a Config. The config carries:
//   - Geometry (c, b, s)
//   - Replacement policy (LRU_MIP / LRU_LIP)
//   - Write policy      (WBWA / WTWNA)
//   - Hit latency       (cycles)
//   - Downstream pointer (next_level Cache, or main_memory MainMemory)
//   - Optional prefetcher + peer_above pointer
//
// We derive `assoc` (ways), `index_bit` (set-index bit count), and
// `num_rows` (set count) from the geometry and pre-allocate the row vector.
Cache::Cache(Config cfg, std::string name)
    : cfg_(std::move(cfg)),
      name_(std::move(name)),
      mshr_(static_cast<std::size_t>(cfg_.mshr_entries)) {
    if (cfg_.c <= cfg_.b + cfg_.s) {
        // Sanity: c > b + s otherwise there are zero sets.
        throw std::invalid_argument(
            "cache geometry: require c > b + s (sets >= 1)");
    }
    // Mirrors project1's sim_setup. e.g. C=10 B=6 S=1 -> 2-way, 8 sets.
    assoc     = 1ULL << cfg_.s;
    index_bit = cfg_.c - cfg_.b - cfg_.s;
    num_rows  = 1ULL << index_bit;

    rows.clear();
    rows.resize(static_cast<std::size_t>(num_rows));
}

// ---------------------------------------------------------------------------
// Address decomposition helpers. Given a byte address or a block address,
// extract the field we need. All three are pure shift/mask ops.
// ---------------------------------------------------------------------------

// block_addr -> tag bits (everything above the index field).
std::uint64_t Cache::get_tag(std::uint64_t block_addr) const {
    return block_addr >> index_bit;
}

// block_addr -> index field (the bits just above the block offset).
std::uint64_t Cache::get_index(std::uint64_t block_addr) const {
    return block_addr & ((1ULL << index_bit) - 1ULL);
}

// byte_addr -> block_addr (drop the block-offset bits).
std::uint64_t Cache::get_block_addr(std::uint64_t addr) const {
    return addr >> cfg_.b;
}

// ---------------------------------------------------------------------------
// Residency queries used by prefetchers.
// ---------------------------------------------------------------------------

// Does the set at `index` contain a valid entry with this tag?
// O(assoc) linear scan — sets are tiny so this is fine.
bool Cache::is_in_cache(std::uint64_t tag, std::uint64_t index) const {
    const auto& set = rows[static_cast<std::size_t>(index)];
    for (auto block = set.LRU_list.begin(); block != set.LRU_list.end(); ++block) {
        if (block->tag == tag && block->valid) {
            return true;
        }
    }
    return false;
}

// Convenience: is the block holding `byte_addr` resident?
bool Cache::block_in(std::uint64_t byte_addr) const {
    const std::uint64_t block_addr = get_block_addr(byte_addr);
    return is_in_cache(get_tag(block_addr), get_index(block_addr));
}

// ---------------------------------------------------------------------------
// Prefetch fill. Called by prefetchers via cache.issue_prefetch(addr).
//   - If the line is already resident, do nothing (don't double-count).
//   - Otherwise allocate it tagged as a prefetched fill so we can later
//     tell whether the prefetch turned into a real demand hit or got
//     evicted unused.
// ---------------------------------------------------------------------------
void Cache::issue_prefetch(std::uint64_t byte_addr) {
    const std::uint64_t block_addr = get_block_addr(byte_addr);
    const std::uint64_t tag        = get_tag(block_addr);
    const std::uint64_t index      = get_index(block_addr);
    if (is_in_cache(tag, index)) {
        return; // already present; nothing to do
    }
    insert_new_block('R', tag, index, block_addr, /*is_prefetch=*/true);
    ++stats_.prefetches_issued;
}

// ---------------------------------------------------------------------------
// insert_new_block: place a freshly fetched (or prefetched) block into the
// set, evicting the LRU if the set is full. Two policies, two paths:
//
//   WBWA  (Write Back, Write Allocate, project1's L1 default):
//     - Always allocates on both read miss AND write miss.
//     - On eviction, if the victim was dirty, writeback to the next level.
//     - A write-miss insert marks the new block dirty immediately.
//
//   WTWNA (Write Through, Write No Allocate, project1's L2 default):
//     - Only allocates on read miss. (Write-miss handling lives in the
//       caller — see access() — and skips this function entirely on a
//       write miss.)
//     - Blocks are never dirty. Writes pass straight through to the next
//       level via a separate path.
//     - Tracks "wasted prefetches": if we evict a block that was prefetched
//       and never touched, count it as a prefetch_miss.
// ---------------------------------------------------------------------------
void Cache::insert_new_block(char rw,
                             std::uint64_t tag,
                             std::uint64_t index,
                             std::uint64_t block_addr,
                             bool is_prefetch) {
    auto& set = rows[static_cast<std::size_t>(index)];

    if (cfg_.write_policy == WritePolicy::WBWA) {
        // ===== WBWA path — mirror of project1's insert_new_block_to_L1 =====
        tag_meta_t new_block;
        new_block.block_addr = block_addr;
        new_block.tag        = tag;
        new_block.valid      = true;
        new_block.dirty      = false;
        new_block.prefetched = is_prefetch;

        if (set.LRU_list.size() == assoc) {
            // Set is full -> evict the LRU (back of the list).
            const auto&  victim         = set.LRU_list.back();
            const bool   victim_dirty   = victim.dirty;
            const auto   victim_block   = victim.block_addr;
            const auto   victim_byte_addr = victim_block << cfg_.b;
            if (cfg_.coherence_sink) {
                // Coherence-managed eviction: notify the sink for both
                // dirty (will become a WRITEBACK) and clean (INVACK)
                // cases. Still bump `writebacks` for dirty victims so
                // the unified-sim stats line up with non-coherent runs.
                if (victim_dirty) ++stats_.writebacks;
                cfg_.coherence_sink->on_evict(victim_block, victim_dirty);
            } else if (victim_dirty) {
                // Non-coherent path: push the dirty line downstream.
                ++stats_.writebacks;
                if (cfg_.next_level) {
                    cfg_.next_level->access(
                        MemReq{victim_byte_addr, Op::Write, /*pc=*/0});
                } else if (cfg_.main_memory) {
                    cfg_.main_memory->access(
                        MemReq{victim_byte_addr, Op::Write, /*pc=*/0});
                }
            }
            set.LRU_list.pop_back();
        }

        if (rw == 'W') {
            // Write-allocate: the inserter wrote to this line, so it's dirty
            // from the moment it lands.
            new_block.dirty = true;
        }

        // MIP inserts at MRU (front). LIP inserts at LRU (back) — that's
        // the trick that protects working-set blocks from one-shot scans.
        if (cfg_.replacement == Replacement::LRU_LIP) {
            set.LRU_list.push_back(new_block);
        } else {
            set.LRU_list.push_front(new_block);
        }
        return;
    }

    // ===== WTWNA path — mirror of project1's insert_new_block_to_L2 =====
    // Only ever called for read misses (write misses skip allocation).
    tag_meta_t new_block;
    new_block.block_addr = block_addr;
    new_block.tag        = tag;
    new_block.valid      = true;
    new_block.dirty      = false; // WTWNA never produces dirty blocks
    new_block.prefetched = is_prefetch;

    if (set.LRU_list.size() == assoc) {
        // Wasted-prefetch accounting: if the victim was prefetched and
        // never got hit by a demand access, the prefetcher mispredicted.
        if (set.LRU_list.back().prefetched) {
            ++stats_.prefetch_misses;
        }
        set.LRU_list.pop_back();
    }

    if (cfg_.replacement == Replacement::LRU_LIP) {
        set.LRU_list.push_back(new_block);
    } else {
        set.LRU_list.push_front(new_block);
    }
}

namespace {

// Helper for the WTWNA write path: scan a set for a tag match and, on
// hit, splice the matched block to the MRU position. Returns end() on
// miss. Mirrors project1's L2 search-and-promote loops.
std::list<tag_meta_t>::iterator
find_and_promote(std::list<tag_meta_t>& lru_list, std::uint64_t tag) {
    for (auto block = lru_list.begin(); block != lru_list.end(); ++block) {
        if (block->tag == tag && block->valid) {
            lru_list.splice(lru_list.begin(), lru_list, block);
            return lru_list.begin();
        }
    }
    return lru_list.end();
}

} // namespace

// ---------------------------------------------------------------------------
// access: the main entry point. Models one demand access (or a writeback
// from the level above, when called from upstream's eviction logic).
//
// Two completely different code paths depending on the write policy:
//
//   WBWA:
//     - Counts every access as either a hit or a miss in stats_.{hits,misses}.
//     - Hit:  splice to MRU; if write, mark dirty; track prefetch_hits if
//             the line was tagged as a prefetched fill.
//     - Miss: count miss, fetch from downstream (always a Read at the
//             next level), allocate via insert_new_block, then ping the
//             prefetcher. Eviction during insert may trigger a writeback.
//
//   WTWNA:
//     - Splits read and write paths. Write hits splice; write misses do
//       nothing (no allocate). Reads count read_hits/read_misses and
//       allocate on miss.
//     - The prefetcher only fires after a demand read miss (matches
//       project1's invocation site).
// ---------------------------------------------------------------------------
AccessResult Cache::access(const MemReq& req) {
    const char rw = rw_of(req.op);

    // Universal counters (both policies). Writes vs reads are tracked so
    // L2 can match project1's separate L2-reads / L2-writes counters.
    ++stats_.accesses;
    if (rw == 'R') ++stats_.reads;
    else if (rw == 'W') ++stats_.writes;

    // Decompose the address into block_addr -> tag + index.
    const std::uint64_t block_addr = get_block_addr(req.addr);
    const std::uint64_t tag        = get_tag(block_addr);
    const std::uint64_t index      = get_index(block_addr);

    auto& set = rows[static_cast<std::size_t>(index)];

    if (cfg_.write_policy == WritePolicy::WBWA) {
        // ===== WBWA access path — mirror of project1's L1 sim_access =====
        bool HIT = false;

        // Linear search through the set. On match: bookkeeping + splice
        // to MRU + break.
        for (auto block = set.LRU_list.begin();
             block != set.LRU_list.end(); ++block) {
            if (block->tag == tag && block->valid) {
                HIT = true;
                if (block->prefetched) {
                    // Demand access on a prefetched line -> the prefetch
                    // paid off. Clear the tag so we don't double-count.
                    ++stats_.prefetch_hits;
                    block->prefetched = false;
                }
                if (rw == 'W') {
                    block->dirty = true; // WBWA: write hit -> dirty
                }
                set.LRU_list.splice(set.LRU_list.begin(), set.LRU_list, block);
                break;
            }
        }

        unsigned downstream_latency = 0;
        bool     suspended          = false;
        if (HIT) {
            ++stats_.hits;
        } else {
            ++stats_.misses;
            // Fetch the line from downstream. Even a write-miss issues a
            // Read at the next level (write-allocate fetches the line
            // before modifying it). Capture the returned latency so the
            // round-trip we report is realistic — pre-Phase-4 we discarded
            // it because --mode cache only counted hits/misses.
            if (cfg_.next_level) {
                const auto sub = cfg_.next_level->access(
                    MemReq{block_addr << cfg_.b, Op::Read, req.pc});
                if (sub.latency == kCoherenceSuspendedLatency) {
                    suspended = true;
                } else {
                    downstream_latency = sub.latency;
                }
            } else if (cfg_.main_memory) {
                downstream_latency = cfg_.main_memory->access(
                    MemReq{block_addr << cfg_.b, Op::Read, req.pc}).latency;
            } else if (cfg_.coherence_sink) {
                // Coherence-managed fetch: the sink will message the
                // directory and call mark_ready when DATA arrives. Skip
                // local block allocation — the adapter will fill via
                // insert_new_block once the data is in.
                cfg_.coherence_sink->on_miss(block_addr, req.op);
                suspended = true;
            }
            if (!suspended) {
                insert_new_block(rw, tag, index, block_addr,
                                 /*is_prefetch=*/false);
                if (cfg_.prefetcher) {
                    cfg_.prefetcher->on_miss(*this, cfg_.peer_above, req);
                }
            }
        }

        AccessResult r;
        r.hit     = HIT;
        r.latency = suspended ? kCoherenceSuspendedLatency
                              : (cfg_.hit_latency + downstream_latency);
        return r;
    }

    // ===== WTWNA access path — mirror of project1's L2 logic =====
    if (rw == 'W') {
        // Project1's write_op_L2: writes hit -> splice; writes miss -> nothing.
        // Notice we do NOT count this in hits/misses or read_hits/read_misses;
        // project1 only reports L2 writes as a flat "writes_l2" total.
        //
        // Quirk vs. the WBWA hit path (line ~289) and the WTWNA read-hit
        // path (line ~351): both of those clear `block.prefetched` and bump
        // `prefetch_hits`. This write-hit path does neither — a prefetched
        // line first touched by a write will not count toward prefetch_hits
        // until/unless it later gets a read. That is faithful to project1's
        // accounting (which is why the regression tests pass); deviating
        // would silently change the pinned numbers, so it stays.
        auto it = find_and_promote(set.LRU_list, tag);
        AccessResult r;
        r.hit     = (it != set.LRU_list.end());
        r.latency = cfg_.hit_latency;
        return r;
    }

    // WTWNA read: same shape as the WBWA hit search but counts go into
    // read_hits / read_misses, matching project1's L2 vocabulary.
    bool HIT = false;
    for (auto block = set.LRU_list.begin();
         block != set.LRU_list.end(); ++block) {
        if (block->tag == tag && block->valid) {
            HIT = true;
            if (block->prefetched) {
                ++stats_.prefetch_hits;
                block->prefetched = false; // demand-touched, clear the tag
            }
            set.LRU_list.splice(set.LRU_list.begin(), set.LRU_list, block);
            break;
        }
    }

    unsigned downstream_latency = 0;
    bool     suspended          = false;
    if (HIT) {
        ++stats_.read_hits;
    } else {
        ++stats_.read_misses;
        // L2 read miss -> allocate locally + fetch from main memory.
        // Order matters for stat fidelity: insert first, then propagate
        // the read to memory, then run the prefetcher (matches project1).
        if (cfg_.next_level) {
            const auto sub = cfg_.next_level->access(
                MemReq{block_addr << cfg_.b, Op::Read, req.pc});
            if (sub.latency == kCoherenceSuspendedLatency) suspended = true;
            else downstream_latency = sub.latency;
        } else if (cfg_.main_memory) {
            downstream_latency = cfg_.main_memory->access(
                MemReq{block_addr << cfg_.b, Op::Read, req.pc}).latency;
        } else if (cfg_.coherence_sink) {
            cfg_.coherence_sink->on_miss(block_addr, req.op);
            suspended = true;
        }
        if (!suspended) {
            insert_new_block('R', tag, index, block_addr, /*is_prefetch=*/false);
            if (cfg_.prefetcher) {
                cfg_.prefetcher->on_miss(*this, cfg_.peer_above, req);
            }
        }
    }

    AccessResult r;
    r.hit     = HIT;
    r.latency = suspended ? kCoherenceSuspendedLatency
                          : (cfg_.hit_latency + downstream_latency);
    return r;
}

// ---------------------------------------------------------------------------
// MSHR / async API. The OoO core uses these to overlap multiple in-flight
// misses; --mode cache never sees them. The cache's hit/miss bookkeeping
// happens immediately at issue (via the synchronous access() above), and
// the MSHR slot just holds the result until `now_` reaches `due_cycle`.
// ---------------------------------------------------------------------------
std::optional<std::uint64_t> Cache::issue(const MemReq& req) {
    const std::uint64_t block_addr = get_block_addr(req.addr);

    // Miss-merge fast-path: if a slot already targets this block, the new
    // request piggybacks. We skip the access() call entirely so that a
    // merged secondary is not double-counted as a separate miss and
    // inherits the primary's due_cycle.
    //
    // Phase 5B: writes are NOT eligible to merge onto a read primary —
    // a write piggybacking a read primary would inherit the primary's
    // (clean) AccessResult and skip the dirty-bit mutation that
    // access() would otherwise apply. Force writes to allocate a
    // fresh slot (which goes through access() below).
    if (req.op != Op::Write) {
        for (const auto& e : mshr_.entries()) {
            if (e.valid && e.block_addr == block_addr) {
                const std::uint64_t id = next_id_++;
                (void)mshr_.allocate(id, block_addr, req.op, req.pc,
                                     e.due_cycle, e.result);
                return id;
            }
        }
    }

    // No merge candidate: we will need a fresh slot. Check capacity FIRST,
    // before access() — access() mutates LRU, may fire prefetchers, and can
    // chain an eviction writeback to the next level. If we found out only
    // afterwards that the MSHR was full and returned nullopt, all of those
    // side effects would still have happened, leaving the cache in a state
    // inconsistent with the caller's "request was rejected" understanding.
    if (mshr_.full()) {
        return std::nullopt;
    }

    const AccessResult result = access(req);
    // Phase 5B: a coherence-managed miss returns latency=∞ as the
    // sentinel for "external completion." Park the MSHR with
    // due_cycle = UINT64_MAX so MSHR::tick never auto-flips ready;
    // CoherenceAdapter calls Cache::mark_ready(id) from on_data_arrival.
    const bool suspended = (result.latency == kCoherenceSuspendedLatency);
    const std::uint64_t due_cycle =
        suspended ? UINT64_MAX : (now_ + result.latency);
    const std::uint64_t id = next_id_++;

    // We pre-checked capacity, so allocate cannot return nullptr here. The
    // (void) cast documents the intentional ignore.
    (void)mshr_.allocate(id, block_addr, req.op, req.pc, due_cycle, result);
    return id;
}

const MSHREntry* Cache::peek(std::uint64_t id) const {
    return mshr_.find(id);
}

void Cache::complete(std::uint64_t id) {
    mshr_.release(id);
}

void Cache::tick() {
    ++now_;
    mshr_.tick(now_);
}

void Cache::mark_ready(std::uint64_t id) {
    if (auto* e = mshr_.find(id)) {
        e->ready = true;
    }
}

void Cache::coherence_invalidate(std::uint64_t block_addr) {
    // Walk just the set the block would land in. If resident, drop it
    // silently (no writeback — the agent's recall path already flushed
    // the dirty data to the directory before we got here).
    const std::uint64_t tag   = get_tag(block_addr);
    const std::uint64_t index = get_index(block_addr);
    auto& set = rows[index];
    for (auto it = set.LRU_list.begin(); it != set.LRU_list.end(); ++it) {
        if (it->valid && it->tag == tag) {
            set.LRU_list.erase(it);
            ++stats_.coherence_invals;
            return;
        }
    }
}

} // namespace comparch::cache
