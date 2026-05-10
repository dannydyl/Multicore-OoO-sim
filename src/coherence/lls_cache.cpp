#include "comparch/coherence/lls_cache.hpp"

#include "comparch/config.hpp"

namespace comparch::coherence {

LlsCache::LlsCache(std::size_t size_blocks, std::size_t assoc)
    : size_blocks_(size_blocks),
      assoc_(assoc),
      num_sets_(0) {
    if (size_blocks_ == 0 || assoc_ == 0) {
        // "Disabled" cache. Every access is a miss with no install.
        // Used by the directory in private_l2 mode so the miss-path
        // code can branch on contains()/access() without an extra
        // null check.
        size_blocks_ = 0;
        assoc_       = 0;
        num_sets_    = 0;
        return;
    }
    if (size_blocks_ % assoc_ != 0) {
        throw ConfigError("LlsCache: size_blocks (" + std::to_string(size_blocks_) +
                          ") must be divisible by assoc (" + std::to_string(assoc_) + ")");
    }
    num_sets_ = size_blocks_ / assoc_;
    sets_.resize(num_sets_);
}

bool LlsCache::contains(BlockId block) const {
    if (num_sets_ == 0) return false;
    const auto& set = sets_[set_of(block)];
    for (auto id : set) {
        if (id == block) return true;
    }
    return false;
}

LlsLookup LlsCache::access(BlockId block) {
    LlsLookup r{};
    if (num_sets_ == 0) {
        // Disabled cache: caller treats this as a permanent miss.
        return r;
    }
    auto& set = sets_[set_of(block)];
    // Hit: move to MRU, no eviction.
    for (auto it = set.begin(); it != set.end(); ++it) {
        if (*it == block) {
            set.splice(set.begin(), set, it);
            r.hit = true;
            return r;
        }
    }
    // Miss + capacity full: evict LRU.
    if (set.size() >= assoc_) {
        r.evicted = true;
        r.victim  = set.back();
        set.pop_back();
    }
    set.push_front(block);
    return r;
}

void LlsCache::invalidate(BlockId block) {
    if (num_sets_ == 0) return;
    auto& set = sets_[set_of(block)];
    for (auto it = set.begin(); it != set.end(); ++it) {
        if (*it == block) { set.erase(it); return; }
    }
}

} // namespace comparch::coherence
