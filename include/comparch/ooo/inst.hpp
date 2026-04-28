#pragma once

// Inst — the per-instruction record that flows through the OoO pipeline.
// =====================================================================
// Models the same five opcode classes as project2's `inst_t` (procsim.hpp:42),
// but builds itself from a ChampSim `trace::Record` instead of project2's
// 11-field text trace.
//
// ChampSim trace limitations (see plan.md §3.1) the classifier works
// around:
//   - No opcode class. We recover BRANCH from `is_branch`, LOAD from
//     `source_memory[]`, STORE from `destination_memory[]`, default to
//     ALU. MUL is NOT recoverable from this format; MUL FUs sit unused
//     unless a PC-bin heuristic is added later.
//   - Multiple memory operands per record. We collapse to a single
//     load/store address (the first non-zero), matching project2's
//     single-address LSU model.
//   - Multiple source/dest registers. Project2 only carries dest+src1+src2;
//     we take the first one or two non-zero entries. Register 0 is
//     treated as "no operand" (matches the project2 driver's mapping at
//     proj2_driver.cpp:231).

#include <array>
#include <cstdint>

#include "comparch/trace.hpp"

namespace comparch::ooo {

enum class Opcode : std::uint8_t {
    Alu    = 0,   // any non-mem, non-branch instruction (default)
    Mul    = 1,   // never produced from ChampSim today; reserved for future
    Load   = 2,
    Store  = 3,
    Branch = 4,
};

// Sentinel for "no operand". Matches project2's int8_t -1 convention.
inline constexpr std::int8_t kNoReg = -1;

struct Inst {
    std::uint64_t pc            = 0;
    Opcode        opcode        = Opcode::Alu;

    // Architectural register numbers. -1 == no operand.
    std::int8_t   dest          = kNoReg;
    std::int8_t   src1          = kNoReg;
    std::int8_t   src2          = kNoReg;

    // Memory address for loads / stores. 0 if not a memory op.
    std::uint64_t mem_addr      = 0;

    // Branch metadata. branch_taken is the ground truth from the trace;
    // predicted_taken is what the predictor guessed at fetch time.
    bool          branch_taken    = false;
    bool          predicted_taken = false;
    bool          mispredict      = false;

    // Sequence number assigned at fetch. Used by the schedule stage for
    // memory disambiguation (loads can only fire when no older store is
    // in flight, etc.).
    std::uint64_t dyn_count     = 0;
};

// Build an Inst from a ChampSim Record. `dyn_count` is the program-order
// sequence number — the caller (fetch stage) increments it on every
// non-stall fetch. The classifier is deterministic and side-effect-free.
Inst classify(const trace::Record& rec, std::uint64_t dyn_count);

// String form of an opcode for stats / debug output.
const char* opcode_name(Opcode op);

} // namespace comparch::ooo
