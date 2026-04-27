#include "comparch/cache/cache.hpp"

#include <stdexcept>
#include <utility>

#include "comparch/cache/main_memory.hpp"

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
            // Evict LRU; if dirty, count writeback and propagate downstream.
            if (set.LRU_list.back().dirty) {
                ++stats_.writebacks;
                const std::uint64_t victim_byte_addr =
                    set.LRU_list.back().block_addr << cfg_.b;
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
            new_block.dirty = true;
        }

        if (cfg_.replacement == Replacement::LRU_LIP) {
            set.LRU_list.push_back(new_block);
        } else {
            set.LRU_list.push_front(new_block);
        }
        return;
    }

    // ===== WTWNA path — mirror of project1's insert_new_block_to_L2 =====
    tag_meta_t new_block;
    new_block.block_addr = block_addr;
    new_block.tag        = tag;
    new_block.valid      = true;
    new_block.dirty      = false; // WTWNA never produces dirty blocks
    new_block.prefetched = is_prefetch;

    if (set.LRU_list.size() == assoc) {
        // Evict LRU. If the victim was a prefetched block that never got
        // touched as a demand hit, count it as a wasted prefetch.
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

// Helper: search a set for a matching valid block. On hit, splice to MRU.
// Returns iterator (end on miss). Mirror of project1's L1/L2 search loops.
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

AccessResult Cache::access(const MemReq& req) {
    const char rw = rw_of(req.op);

    ++stats_.accesses;
    if (rw == 'R') ++stats_.reads;
    else if (rw == 'W') ++stats_.writes;

    const std::uint64_t block_addr = get_block_addr(req.addr);
    const std::uint64_t tag        = get_tag(block_addr);
    const std::uint64_t index      = get_index(block_addr);

    auto& set = rows[static_cast<std::size_t>(index)];

    if (cfg_.write_policy == WritePolicy::WBWA) {
        // ===== WBWA access path — mirror of project1's L1 sim_access =====
        bool HIT = false;

        for (auto block = set.LRU_list.begin();
             block != set.LRU_list.end(); ++block) {
            if (block->tag == tag && block->valid) {
                HIT = true;
                if (rw == 'W') {
                    block->dirty = true; // WBWA: write hit -> dirty
                }
                set.LRU_list.splice(set.LRU_list.begin(), set.LRU_list, block);
                break;
            }
        }

        if (HIT) {
            ++stats_.hits;
        } else {
            ++stats_.misses;
            // L1 miss: fill from next level (always a read at the next
            // level — write-allocate also issues a read fetch).
            if (cfg_.next_level) {
                cfg_.next_level->access(
                    MemReq{block_addr << cfg_.b, Op::Read, req.pc});
            } else if (cfg_.main_memory) {
                cfg_.main_memory->access(
                    MemReq{block_addr << cfg_.b, Op::Read, req.pc});
            }
            insert_new_block(rw, tag, index, block_addr, /*is_prefetch=*/false);
        }

        AccessResult r;
        r.hit     = HIT;
        r.latency = cfg_.hit_latency;
        return r;
    }

    // ===== WTWNA access path — mirror of project1's L2 logic =====
    if (rw == 'W') {
        // WTWNA write: project1's write_op_L2.
        //   - Search; on hit, splice to MRU (no dirty update).
        //   - On miss, do nothing (no allocate).
        auto it = find_and_promote(set.LRU_list, tag);
        AccessResult r;
        r.hit     = (it != set.LRU_list.end());
        r.latency = cfg_.hit_latency;
        return r;
    }

    // WTWNA read: project1's L2 read path inside sim_access.
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

    if (HIT) {
        ++stats_.read_hits;
    } else {
        ++stats_.read_misses;
        insert_new_block('R', tag, index, block_addr, /*is_prefetch=*/false);
        // L2 miss => fill from main memory (next level beyond L2).
        if (cfg_.next_level) {
            cfg_.next_level->access(
                MemReq{block_addr << cfg_.b, Op::Read, req.pc});
        } else if (cfg_.main_memory) {
            cfg_.main_memory->access(
                MemReq{block_addr << cfg_.b, Op::Read, req.pc});
        }
    }

    AccessResult r;
    r.hit     = HIT;
    r.latency = cfg_.hit_latency;
    return r;
}

} // namespace comparch::cache
