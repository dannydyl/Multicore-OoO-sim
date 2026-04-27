#include "comparch/cache/cache.hpp"

#include <stdexcept>
#include <utility>

namespace comparch::cache {

Cache::Cache(Config cfg, std::string name)
    : cfg_(cfg), name_(std::move(name)) {
    if (cfg_.c <= cfg_.b + cfg_.s) {
        throw std::invalid_argument(
            "cache geometry: require c > b + s (sets >= 1)");
    }
    // Derived geometry, mirrors project1's sim_setup.
    assoc     = 1ULL << cfg_.s;
    index_bit = cfg_.c - cfg_.b - cfg_.s;
    num_rows  = 1ULL << index_bit;

    rows.clear();
    rows.resize(static_cast<std::size_t>(num_rows));
}

std::uint64_t Cache::get_tag(std::uint64_t block_addr) const {
    return block_addr >> index_bit;
}

std::uint64_t Cache::get_index(std::uint64_t block_addr) const {
    return block_addr & ((1ULL << index_bit) - 1ULL);
}

std::uint64_t Cache::get_block_addr(std::uint64_t addr) const {
    return addr >> cfg_.b;
}

bool Cache::is_in_cache(std::uint64_t tag, std::uint64_t index) const {
    const auto& set = rows[static_cast<std::size_t>(index)];
    for (auto block = set.LRU_list.begin(); block != set.LRU_list.end(); ++block) {
        if (block->tag == tag && block->valid) {
            return true;
        }
    }
    return false;
}

// Mirrors project1's insert_new_block_to_L1 (WBWA path).
// WTWNA path arrives in Phase 2.3 alongside L2.
void Cache::insert_new_block(char rw,
                             std::uint64_t tag,
                             std::uint64_t index,
                             std::uint64_t block_addr,
                             bool is_prefetch) {
    if (cfg_.write_policy != WritePolicy::WBWA) {
        // WTWNA path lands in a later commit (Phase 2.3 with L2).
        throw std::logic_error("WTWNA insert path not implemented yet");
    }

    tag_meta_t new_block;
    new_block.block_addr = block_addr;
    new_block.tag        = tag;
    new_block.valid      = true;
    new_block.dirty      = false;
    new_block.prefetched = is_prefetch;

    auto& set = rows[static_cast<std::size_t>(index)];

    // ---------- WBWA: cache gets a new block ----------
    if (set.LRU_list.size() == assoc) {
        // Evict LRU; if dirty, count a writeback. Project1's L1 logic.
        if (set.LRU_list.back().dirty) {
            ++stats_.writebacks;
            // Propagation to next level lands in Phase 2.3 with the L2 hookup.
        }
        set.LRU_list.pop_back();
    }

    // WBWA: a write hit causes the block to become dirty immediately.
    if (rw == 'W') {
        new_block.dirty = true;
    }

    // MIP only for now (matches project1's L1 hardcoded behavior).
    set.LRU_list.push_front(new_block);
}

AccessResult Cache::access(const MemReq& req) {
    const char rw = rw_of(req.op);

    ++stats_.accesses;
    if (rw == 'R') ++stats_.reads;
    else if (rw == 'W') ++stats_.writes;

    const std::uint64_t block_addr = get_block_addr(req.addr);
    const std::uint64_t tag        = get_tag(block_addr);
    const std::uint64_t index      = get_index(block_addr);

    auto& set = rows[static_cast<std::size_t>(index)];

    bool HIT = false;

    // Look up in the set; mirror of project1's L1 search loop.
    for (auto block = set.LRU_list.begin(); block != set.LRU_list.end(); ++block) {
        if (block->tag == tag && block->valid) {
            HIT = true;
            if (rw == 'W') {
                block->dirty = true; // WBWA: write hit makes block dirty
            }
            // Update LRU: splice the hit block to MRU (front).
            set.LRU_list.splice(set.LRU_list.begin(), set.LRU_list, block);
            break;
        }
    }

    if (HIT) {
        ++stats_.hits;
    } else {
        ++stats_.misses;
        // L1 miss servicing path. Project1 dispatched to L2 here when L2
        // was enabled. With next_level == nullptr (this commit), we just
        // allocate the block locally — the miss-fill data is "served by
        // memory" implicitly. Real L2 chaining lands in Phase 2.3.
        insert_new_block(rw, tag, index, block_addr, /*is_prefetch=*/false);
    }

    AccessResult r;
    r.hit     = HIT;
    r.latency = cfg_.hit_latency;
    return r;
}

} // namespace comparch::cache
