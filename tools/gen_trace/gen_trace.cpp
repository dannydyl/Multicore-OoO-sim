#include "gen_trace.hpp"

#include <random>

namespace comparch::gen_trace {

namespace {

std::uint8_t random_reg(std::mt19937_64& rng) {
    std::uniform_int_distribution<unsigned> d(1, 31);
    return static_cast<std::uint8_t>(d(rng));
}

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

std::uint64_t addr_for(const GenParams& p, std::size_t i, std::mt19937_64& rng) {
    switch (p.pattern) {
        case Pattern::Sequential:
            return p.addr_base + (static_cast<std::uint64_t>(i) * p.addr_stride);
        case Pattern::Stream:
            return p.addr_base + (static_cast<std::uint64_t>(i) * p.addr_stride);
        case Pattern::Loop:
            return p.addr_base + ((static_cast<std::uint64_t>(i) % p.loop_size) * p.addr_stride);
        case Pattern::Random:
            return p.addr_base + (rng() & 0x00FF'FFC0ULL);
    }
    return p.addr_base;
}

} // namespace

void generate_records(const GenParams& p, trace::Writer& w) {
    std::mt19937_64 rng(p.seed);
    std::uniform_real_distribution<double> u01(0.0, 1.0);

    for (std::size_t i = 0; i < p.records; ++i) {
        trace::Record r{};
        r.ip = pc_for(p, i, rng);

        const double dice = u01(rng);
        if (dice < p.branch_rate) {
            r.is_branch    = true;
            r.branch_taken = (u01(rng) < 0.5);
            r.source_registers[0] = random_reg(rng);
        } else {
            const double rest = (dice - p.branch_rate) /
                                std::max(1e-9, 1.0 - p.branch_rate);
            if (rest < p.load_rate) {
                r.source_memory[0]         = addr_for(p, i, rng);
                r.source_registers[0]      = random_reg(rng);
                r.destination_registers[0] = random_reg(rng);
            } else if (rest < p.load_rate + p.store_rate) {
                r.destination_memory[0] = addr_for(p, i, rng);
                r.source_registers[0]   = random_reg(rng);
                r.source_registers[1]   = random_reg(rng);
            } else {
                r.source_registers[0]      = random_reg(rng);
                r.source_registers[1]      = random_reg(rng);
                r.destination_registers[0] = random_reg(rng);
            }
        }

        w.write(r);
    }
}

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
