#pragma once

// ROB — Reorder Buffer.
// =====================
// Direct port of project2's circular ROB (procsim.cpp:115-164). Holds
// in-flight instructions in program order so retirement can be performed
// in-order even when execution is out-of-order.
//
// Each entry remembers the destination tag (so retire can broadcast to
// the RAT), the dynamic count, the predicted/actual branch outcome (so
// retire can detect mispredicts), and a `ready` flag set by the exec
// stage when the instruction has produced its result.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "comparch/ooo/inst.hpp"

namespace comparch::ooo {

struct RobEntry {
    bool          ready     = false;
    std::int8_t   dest_reg  = kNoReg;
    std::uint64_t dest_tag  = 0;
    Inst          inst{};
};

class Rob {
public:
    explicit Rob(std::size_t capacity);

    // Append a new entry at the tail. Returns the entry's index, which
    // the schedQ entry stashes for later "set ready" calls from exec.
    // Pre-condition: !full().
    std::size_t allocate(const RobEntry& e);

    // Mutable access by index (exec uses this to flip `ready`).
    RobEntry&       operator[](std::size_t idx)       { return entries_[idx]; }
    const RobEntry& operator[](std::size_t idx) const { return entries_[idx]; }

    // Head accessors used by retire.
    const RobEntry& head_entry() const { return entries_[head_]; }
    void            retire_head();

    bool        full()      const { return count_ == capacity_; }
    bool        empty()     const { return count_ == 0; }
    std::size_t size()      const { return count_; }
    std::size_t capacity()  const { return capacity_; }

    // For unit tests / introspection.
    std::size_t head_index() const { return head_; }

private:
    std::vector<RobEntry> entries_;
    std::size_t           capacity_;
    std::size_t           head_  = 0;
    std::size_t           tail_  = 0;
    std::size_t           count_ = 0;
};

} // namespace comparch::ooo
