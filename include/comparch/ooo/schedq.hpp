#pragma once

// SchedQ — Scheduling Queue (a.k.a. reservation stations).
// ========================================================
// Direct port of project2's std::list<schedQ_t> (procsim.cpp:95-113).
// Each entry holds an in-flight instruction's source operand status,
// destination tag, and a pointer back to its ROB slot. Dispatch pushes,
// schedule selects oldest-ready, exec wakes dependents and erases.
//
// We keep std::list<SchedEntry> as the underlying container so that:
//   - entries have stable addresses (FU "schedQ_ptr" fields are raw
//     pointers; std::list never invalidates iterators or pointers
//     except for the erased element);
//   - erase-on-completion is O(1) given an iterator/pointer.
//
// Memory disambiguation rules (project2's stage_schedule, lines 596-620)
// apply at fire time, not at queue insertion: a load can fire only if no
// older store is in the queue; a store only if no older mem op is in
// the queue.

#include <cstddef>
#include <cstdint>
#include <list>

#include "comparch/ooo/inst.hpp"

namespace comparch::ooo {

// One source-operand slot. Mirrors project2's `src_t` (procsim.cpp:96).
struct SrcOperand {
    std::uint64_t tag   = 0;
    bool          ready = true;
};

// One scheduling-queue entry. Carries the full Inst metadata so retire,
// exec, and schedule all see the same view of the instruction.
struct SchedEntry {
    Inst          inst{};
    SrcOperand    src1{};
    SrcOperand    src2{};
    std::uint64_t dest_tag = 0;
    std::size_t   rob_idx  = 0;
    bool          busy     = false;   // currently executing in an FU
};

class SchedQ {
public:
    explicit SchedQ(std::size_t capacity);

    bool        full()     const { return entries_.size() >= capacity_; }
    bool        empty()    const { return entries_.empty(); }
    std::size_t size()     const { return entries_.size(); }
    std::size_t capacity() const { return capacity_; }

    // Dispatch path. Caller has already confirmed !full().
    SchedEntry* push_back(const SchedEntry& e);

    // Erase the entry with this dest_tag (called by exec on completion).
    // Returns true if a match was found.
    bool erase_by_tag(std::uint64_t dest_tag);

    // CDB broadcast: every src in the queue whose tag matches `result_tag`
    // gets marked ready. Project2 calls this for ALU, MUL stage3, and
    // load completion (procsim.cpp:361-368, 397-404, 463-470).
    void wake_dependents(std::uint64_t result_tag);

    // Direct access for the schedule stage — it iterates to find the
    // oldest ready instruction of each FU's opcode class.
    std::list<SchedEntry>&       entries()       { return entries_; }
    const std::list<SchedEntry>& entries() const { return entries_; }

private:
    std::list<SchedEntry> entries_;
    std::size_t           capacity_;
};

} // namespace comparch::ooo
