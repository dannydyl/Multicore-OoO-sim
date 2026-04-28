// gen_trace
// =========
// Synthesizes ChampSim Standard binary traces (.champsimtrace) for testing
// the cache / predictor / OoO subsystems without depending on a real
// dynamic-binary-instrumentation pipeline (DynamoRIO, Pin).
//
// The generator does NOT model any actual program. It just emits
// 64-byte records in a chosen pattern: monotonic PCs, looping PCs,
// strided addresses, or pure random. Useful for:
//   - Unit tests where you need a known input shape
//   - Sanity-checking a sim feature (e.g. "does +1 prefetch produce hits
//     on a stream pattern?" — yes, every other access)
//   - Exploring cache behavior interactively before plugging in a real
//     trace from a public corpus

#include "gen_trace.hpp"

#include <random>

namespace comparch::gen_trace {

namespace {

// Random architectural register ID in [1, 31]. Skips reg 0 because the
// ChampSim format reserves 0 to mean "slot unused" (not "architectural
// register r0"). See docs/trace-format.md.
std::uint8_t random_reg(std::mt19937_64& rng) {
    std::uniform_int_distribution<unsigned> d(1, 31);
    return static_cast<std::uint8_t>(d(rng));
}

// Compute the PC for record `i` according to the chosen pattern.
//   Sequential / Stream: PC marches forward at a fixed stride (4 bytes
//                        by default — one instruction in a fixed-width ISA).
//   Loop:                PC cycles within a small range, simulating a
//                        tight loop body. Useful for branch predictor work.
//   Random:              uniformly random within a 1 MB window above
//                        pc_base — worst case for any predictor.
std::uint64_t pc_for(const GenParams& p, std::size_t i, std::mt19937_64& rng) {
    switch (p.pattern) {
        case Pattern::Sequential:
        case Pattern::Stream:
            return p.pc_base + static_cast<std::uint64_t>(i) * p.pc_stride;
        case Pattern::Loop: {
            const std::uint64_t k = i % p.loop_size;
            return p.pc_base + k * p.pc_stride;
        }
        case Pattern::Random:
            return p.pc_base + (rng() & 0x000F'FFFFULL);
    }
    return p.pc_base;
}

// Compute the data address for record `i` according to the chosen pattern.
//   Sequential / Stream: addr marches forward at addr_stride (default 64,
//                        one cache block). Stream is the prefetcher's
//                        easy mode — perfect spatial locality.
//   Loop:                addr cycles within `loop_size` distinct blocks.
//                        High temporal locality, exercises hit paths.
//   Random:              uniformly random aligned to 64 bytes.
std::uint64_t addr_for(const GenParams& p, std::size_t i, std::mt19937_64& rng) {
    switch (p.pattern) {
        case Pattern::Sequential:
            return p.addr_base + (static_cast<std::uint64_t>(i) * p.addr_stride);
        case Pattern::Stream:
            return p.addr_base + (static_cast<std::uint64_t>(i) * p.addr_stride);
        case Pattern::Loop:
            return p.addr_base + ((static_cast<std::uint64_t>(i) % p.loop_size) * p.addr_stride);
        case Pattern::Random:
            // Mask down to a 16 MB window, aligned to 64 B (low 6 bits zero).
            return p.addr_base + (rng() & 0x00FF'FFC0ULL);
    }
    return p.addr_base;
}

} // namespace

// Generate `p.records` ChampSim Standard records and write them to `w`.
//
// Each iteration:
//   1. Build an empty 64-byte Record.
//   2. Fill in the PC.
//   3. Roll a die in [0, 1) and partition by rate:
//        dice < branch_rate                          -> branch
//        otherwise, partition the remaining (1 - branch_rate) probability
//        among load / store / generic-ALU according to load_rate and
//        store_rate. So load_rate=0.4 means "40% of NON-BRANCH records
//        are loads", not "40% of all records are loads".
//   4. Populate the right metadata fields (memory addresses for loads/stores,
//      register slots for ALU/branches).
//   5. Emit the record.
//
// The RNG is seeded from p.seed so output is reproducible across runs
// when the same params are used.
void generate_records(const GenParams& p, trace::Writer& w) {
    std::mt19937_64 rng(p.seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    for (std::size_t i = 0; i < p.records; ++i) {
        trace::Record r{};
        r.ip = pc_for(p, i, rng);

        const double dice = u01(rng);
        if (dice < p.branch_rate) {
            // Branch instruction: just one source reg (the comparison reg).
            r.is_branch    = true;
            r.branch_taken = (u01(rng) < 0.5);
            r.source_registers[0] = random_reg(rng);
        } else {
            // Non-branch: re-roll within the (1 - branch_rate) slice and
            // pick load / store / ALU.
            const double rest = (dice - p.branch_rate) /
                                std::max(1e-9, 1.0 - p.branch_rate);
            if (rest < p.load_rate) {
                // Load: 1 source reg (address base), 1 dest reg (loaded value),
                //       1 source memory address.
                r.source_memory[0]         = addr_for(p, i, rng);
                r.source_registers[0]      = random_reg(rng);
                r.destination_registers[0] = random_reg(rng);
            } else if (rest < p.load_rate + p.store_rate) {
                // Store: 2 source regs (address base + value), 1 dest memory.
                r.destination_memory[0] = addr_for(p, i, rng);
                r.source_registers[0]   = random_reg(rng);
                r.source_registers[1]   = random_reg(rng);
            } else {
                // Generic ALU op: 2 source regs, 1 dest reg, no memory.
                r.source_registers[0]      = random_reg(rng);
                r.source_registers[1]      = random_reg(rng);
                r.destination_registers[0] = random_reg(rng);
            }
        }

        w.write(r);
    }
}

// String <-> Pattern conversions for the CLI parser. Returns nullopt on
// an unknown name so the caller can print a useful error.
std::optional<Pattern> parse_pattern(std::string_view s) {
    if (s == "sequential") return Pattern::Sequential;
    if (s == "loop")       return Pattern::Loop;
    if (s == "stream")     return Pattern::Stream;
    if (s == "random")     return Pattern::Random;
    return std::nullopt;
}

std::string_view pattern_name(Pattern p) {
    switch (p) {
        case Pattern::Sequential: return "sequential";
        case Pattern::Loop:       return "loop";
        case Pattern::Stream:     return "stream";
        case Pattern::Random:     return "random";
    }
    return "?";
}

} // namespace comparch::gen_trace
