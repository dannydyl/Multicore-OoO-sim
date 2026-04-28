#include "comparch/ooo/inst.hpp"

namespace comparch::ooo {

namespace {

// First non-zero entry in a fixed-size array, or kNoReg if all zero.
// project2 maps register 0 to -1 (proj2_driver.cpp:231); ChampSim does
// the same convention, treating 0 as "unused slot".
template <std::size_t N>
std::int8_t first_nonzero_reg(const std::array<std::uint8_t, N>& regs) {
    for (auto r : regs) {
        if (r != 0) return static_cast<std::int8_t>(r);
    }
    return kNoReg;
}

template <std::size_t N>
std::int8_t second_nonzero_reg(const std::array<std::uint8_t, N>& regs) {
    bool found_first = false;
    for (auto r : regs) {
        if (r == 0) continue;
        if (!found_first) {
            found_first = true;
            continue;
        }
        return static_cast<std::int8_t>(r);
    }
    return kNoReg;
}

template <std::size_t N>
std::uint64_t first_nonzero_addr(const std::array<std::uint64_t, N>& addrs) {
    for (auto a : addrs) {
        if (a != 0) return a;
    }
    return 0;
}

} // namespace

Inst classify(const trace::Record& rec, std::uint64_t dyn_count) {
    Inst inst;
    inst.pc        = rec.ip;
    inst.dyn_count = dyn_count;

    // Opcode priority: BRANCH > STORE > LOAD > ALU. STORE wins over LOAD
    // for ChampSim records that somehow set both (rare; ChampSim itself
    // emits one or the other per record).
    const std::uint64_t store_addr = first_nonzero_addr(rec.destination_memory);
    const std::uint64_t load_addr  = first_nonzero_addr(rec.source_memory);

    if (rec.is_branch) {
        inst.opcode       = Opcode::Branch;
        inst.branch_taken = rec.branch_taken;
    } else if (store_addr != 0) {
        inst.opcode   = Opcode::Store;
        inst.mem_addr = store_addr;
    } else if (load_addr != 0) {
        inst.opcode   = Opcode::Load;
        inst.mem_addr = load_addr;
    } else {
        inst.opcode = Opcode::Alu;
    }

    // Operand registers. Project2's single-dest, two-src model maps
    // cleanly onto ChampSim's first non-zero entries.
    inst.dest = first_nonzero_reg(rec.destination_registers);
    inst.src1 = first_nonzero_reg(rec.source_registers);
    inst.src2 = second_nonzero_reg(rec.source_registers);

    return inst;
}

const char* opcode_name(Opcode op) {
    switch (op) {
        case Opcode::Alu:    return "ALU";
        case Opcode::Mul:    return "MUL";
        case Opcode::Load:   return "LOAD";
        case Opcode::Store:  return "STORE";
        case Opcode::Branch: return "BRANCH";
    }
    return "?";
}

} // namespace comparch::ooo
