#pragma once

// MSHR — Miss-Status Holding Register table.
// =========================================
// One slot per in-flight miss. The OoO LSU calls `Cache::issue()` to start
// a memory request, gets back an id, then polls `Cache::peek(id)` each
// cycle until `ready == true`. The MSHR is the data structure that holds
// the request between issue and ready.
//
// Three responsibilities:
//   1. Hand out slots on `allocate(...)`.
//   2. Walk the table on `tick(now)` and flip `ready` for any entry whose
//      `due_cycle` has been reached.
//   3. Merge two requests to the same block onto a single slot — the
//      second request piggybacks via `merged_ids` and gets its data when
//      the primary's miss returns.
//
// Sized by the cache's `mshr_entries` config. When the table is full,
// `allocate` returns nullptr and the caller (LSU / fetch) stalls.
//
// Reference: Kroft 1981, "Lockup-Free Instruction Fetch/Prefetch Cache
// Organization" (ISCA-8) — origin of the MSHR concept.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "comparch/cache/mem_req.hpp"

namespace comparch::cache {

// One MSHR slot. Lives in MSHR::table_ at a fixed index for the entry's
// lifetime; freed on `release` of the last holder.
struct MSHREntry {
    std::uint64_t id          = 0;       // primary request id
    std::uint64_t block_addr  = 0;       // key for miss merging
    Op            op          = Op::Read;
    std::uint64_t pc          = 0;

    bool          valid       = false;   // slot occupied
    bool          ready       = false;   // request has been serviced
    std::uint64_t due_cycle   = 0;       // tick() compares against `now`
    AccessResult  result{};               // populated at allocate-time

    // Secondary requests merged onto this slot (same block, allocated
    // while this miss was in flight). Each gets its own id and is
    // separately addressable via MSHR::find().
    std::vector<std::uint64_t> merged_ids;
};

class MSHR {
public:
    explicit MSHR(std::size_t num_entries);

    // Try to allocate a slot for a request to `block_addr`.
    //
    //   - If a valid entry already targets this block, the new id merges
    //     onto that entry (added to merged_ids) and the existing entry is
    //     returned. `due_cycle` and `result` arguments are ignored on
    //     merge — the secondary inherits the primary's timing.
    //
    //   - Otherwise, finds a free slot, populates it, returns it.
    //
    //   - If no merge match AND no free slot, returns nullptr (caller
    //     must stall).
    MSHREntry* allocate(std::uint64_t       id,
                        std::uint64_t       block_addr,
                        Op                  op,
                        std::uint64_t       pc,
                        std::uint64_t       due_cycle,
                        const AccessResult& result);

    // Look up the slot holding this id, either as primary (entry.id == id)
    // or as a merged secondary.
    MSHREntry*       find(std::uint64_t id);
    const MSHREntry* find(std::uint64_t id) const;

    // Advance pending entries: any valid entry whose due_cycle has been
    // reached flips to ready.
    void tick(std::uint64_t now);

    // Release this id's hold on its slot.
    //   - If id was the primary and any merged secondaries remain, the
    //     first secondary is promoted to primary and the slot stays live.
    //   - If id was the primary and no secondaries remain, the slot frees.
    //   - If id was a secondary, just removes it from merged_ids.
    //   - Unknown id: silent no-op.
    void release(std::uint64_t id);

    bool        full()      const;
    std::size_t occupancy() const;
    std::size_t capacity()  const { return table_.size(); }

    // Iteration support for tests (and future stats dumps).
    const std::vector<MSHREntry>& entries() const { return table_; }

private:
    // Fixed-size table. `valid == false` means the slot is free.
    // Linear scan for free / matching block — table is small (typically
    // 4–16 entries), so the constant-factor difference vs. a hashmap is
    // negligible and the simpler structure makes the test cases obvious.
    std::vector<MSHREntry> table_;
};

} // namespace comparch::cache
