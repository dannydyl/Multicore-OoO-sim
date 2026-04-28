#include "comparch/cache/mshr.hpp"

#include <algorithm>

namespace comparch::cache {

MSHR::MSHR(std::size_t num_entries) : table_(num_entries) {}

namespace {

// True iff `e` is currently holding `id` either as primary or as a merged
// secondary. Used by find() and release().
bool entry_holds(const MSHREntry& e, std::uint64_t id) {
    if (!e.valid) return false;
    if (e.id == id) return true;
    return std::find(e.merged_ids.begin(), e.merged_ids.end(), id)
           != e.merged_ids.end();
}

} // namespace

MSHREntry* MSHR::allocate(std::uint64_t       id,
                          std::uint64_t       block_addr,
                          Op                  op,
                          std::uint64_t       pc,
                          std::uint64_t       due_cycle,
                          const AccessResult& result) {
    // Step 1: miss-merge. If a valid entry already targets this block, the
    // secondary piggybacks. It inherits the primary's due_cycle and result.
    for (auto& e : table_) {
        if (e.valid && e.block_addr == block_addr) {
            e.merged_ids.push_back(id);
            return &e;
        }
    }

    // Step 2: find a free slot.
    for (auto& e : table_) {
        if (!e.valid) {
            e             = MSHREntry{};
            e.id          = id;
            e.block_addr  = block_addr;
            e.op          = op;
            e.pc          = pc;
            e.valid       = true;
            e.ready       = false;
            e.due_cycle   = due_cycle;
            e.result      = result;
            return &e;
        }
    }

    // Step 3: full. Caller stalls.
    return nullptr;
}

MSHREntry* MSHR::find(std::uint64_t id) {
    for (auto& e : table_) {
        if (entry_holds(e, id)) return &e;
    }
    return nullptr;
}

const MSHREntry* MSHR::find(std::uint64_t id) const {
    for (const auto& e : table_) {
        if (entry_holds(e, id)) return &e;
    }
    return nullptr;
}

void MSHR::tick(std::uint64_t now) {
    for (auto& e : table_) {
        if (e.valid && !e.ready && now >= e.due_cycle) {
            e.ready = true;
        }
    }
}

void MSHR::release(std::uint64_t id) {
    for (auto& e : table_) {
        if (!e.valid) continue;

        if (e.id == id) {
            // Primary release. If secondaries remain, promote the first
            // one to primary so the slot stays addressable for them. Else
            // free the slot outright.
            if (e.merged_ids.empty()) {
                e.valid = false;
                e.ready = false;
                e.merged_ids.clear();
            } else {
                e.id = e.merged_ids.front();
                e.merged_ids.erase(e.merged_ids.begin());
            }
            return;
        }

        auto it = std::find(e.merged_ids.begin(), e.merged_ids.end(), id);
        if (it != e.merged_ids.end()) {
            e.merged_ids.erase(it);
            return;
        }
    }
    // Unknown id: silent no-op.
}

bool MSHR::full() const {
    for (const auto& e : table_) {
        if (!e.valid) return false;
    }
    return true;
}

std::size_t MSHR::occupancy() const {
    std::size_t n = 0;
    for (const auto& e : table_) {
        if (e.valid) ++n;
    }
    return n;
}

} // namespace comparch::cache
