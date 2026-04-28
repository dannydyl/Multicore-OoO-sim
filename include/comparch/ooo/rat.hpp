#pragma once

// RAT — Register Alias Table.
// ===========================
// Direct port of project2's "messy register file" (procsim.cpp:40-83).
// Each architectural register has one entry holding (a) the most recent
// physical-register tag dispatched into it and (b) a ready bit indicating
// whether the value is available on the CDB yet.
//
// Lifecycle of a tag (matches project2's tag_counter scheme):
//   - On dispatch, allocate a fresh tag, write_use(dest, tag): tag
//     becomes pending (ready=false).
//   - On writeback, mark_complete(dest, tag) flips ready=true *only* if
//     the entry's tag still equals the writeback's tag. A stale tag (a
//     newer dispatch overwrote it) is silently ignored — that completion
//     belongs to flushed speculative state.
//   - On misprediction, flush_to_ready() sets every entry to ready=true.
//     This discards in-flight RAW deps; the existing project2 design
//     does this because everything younger than the mispredicted branch
//     is dropped.
//
// No explicit free list. Tags are a monotonic 64-bit counter shared
// across the whole pipeline (allocate_tag()) — large enough to never
// recycle in any realistic run.

#include <cstdint>
#include <vector>

#include "comparch/ooo/inst.hpp"

namespace comparch::ooo {

inline constexpr std::size_t kNumArchRegs = 32;  // project2's NUM_REGS

struct RatEntry {
    std::uint64_t tag   = 0;
    bool          ready = true;
};

class Rat {
public:
    Rat();

    // Read the current (tag, ready) for an architectural register.
    // For kNoReg (-1, "no operand"), returns {tag=0, ready=true} so the
    // caller can treat absent operands as immediately-ready dependencies.
    RatEntry read(std::int8_t addr) const;

    // Allocate `tag` to `addr` as the new in-flight destination. Marks
    // the entry ready=false. No-op if addr == kNoReg.
    void write_use(std::int8_t addr, std::uint64_t tag);

    // Writeback completion. If addr's entry still carries `tag`, flip it
    // ready=true. Stale (overwritten) tags are silently ignored.
    void mark_complete(std::int8_t addr, std::uint64_t tag);

    // Flush everything to ready (used on retired misprediction).
    void flush_to_ready();

    // Allocate a fresh, monotonically-increasing tag. Tag 0 is reserved
    // as "no operand" sentinel and is never returned. Initial tags
    // 1..kNumArchRegs are used as the architectural seed values so that
    // a freshly-constructed Rat reads ready=true for every register.
    std::uint64_t allocate_tag();

    // Test introspection.
    const RatEntry& entry(std::size_t idx) const { return entries_[idx]; }
    std::size_t     size()                  const { return entries_.size(); }

private:
    std::vector<RatEntry> entries_;
    std::uint64_t         next_tag_ = static_cast<std::uint64_t>(kNumArchRegs) + 1;
};

} // namespace comparch::ooo
