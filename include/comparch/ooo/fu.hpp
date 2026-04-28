#pragma once

// FU — Function Unit tables.
// ==========================
// Direct port of project2's three FU types (procsim.cpp:166-225). Each
// pipeline carries an array of identical units; the schedule stage hands
// a SchedEntry to a free unit, and the execute stage advances or
// completes them.
//
//   ALU   1-stage,  fires single-cycle. project2's ALU_t.
//   MUL   3-stage pipelined, no stalls. project2's MUL_t.
//   LSU   variable-latency for loads (cache-driven), 1-cycle for stores.
//         The OoO core's LSU now consults the L1-D MSHR via cache::Cache
//         (Phase 4), replacing project2's pre-baked left_cycles counter
//         that read from the trace's dcache_hit field.

#include <array>
#include <cstdint>
#include <vector>

#include "comparch/ooo/schedq.hpp"

namespace comparch::ooo {

struct AluUnit {
    bool        busy      = false;
    SchedEntry* sched_ptr = nullptr;
};

// 3-stage pipelined multiplier. busy_stage[0] = newly-issued, [2] = about
// to retire from the MUL pipe. Each stage has its own SchedEntry pointer
// so we can wake dependents at the right moment (only at stage 3).
struct MulUnit {
    std::array<bool,        3> busy_stage{};
    std::array<SchedEntry*, 3> sched_at_stage{};
};

// LSU. For loads, `mshr_id` indexes into l1d's MSHR table; the exec
// stage polls Cache::peek(mshr_id) and completes when ready==true. For
// stores, mshr_id is unused (store completes synchronously on issue,
// matching project2 — store-buffer / TSO is a Phase 5+ concern).
struct LsuUnit {
    bool          busy      = false;
    bool          is_load   = false;
    std::uint64_t mshr_id   = 0;
    SchedEntry*   sched_ptr = nullptr;
};

inline std::size_t free_alu_count(const std::vector<AluUnit>& units) {
    std::size_t n = 0;
    for (const auto& u : units) if (!u.busy) ++n;
    return n;
}

inline std::size_t free_mul_count(const std::vector<MulUnit>& units) {
    // Only stage 0 (the newly-issued slot) needs to be free for a MUL
    // to be dispatched. Stages 1/2 having something in flight doesn't
    // block a new issue.
    std::size_t n = 0;
    for (const auto& u : units) if (!u.busy_stage[0]) ++n;
    return n;
}

inline std::size_t free_lsu_count(const std::vector<LsuUnit>& units) {
    std::size_t n = 0;
    for (const auto& u : units) if (!u.busy) ++n;
    return n;
}

} // namespace comparch::ooo
