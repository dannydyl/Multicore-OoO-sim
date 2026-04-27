#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "comparch/trace.hpp"

namespace comparch::gen_trace {

enum class Pattern {
    Sequential, // monotonic PC, varied access addresses
    Loop,       // small PC range cycled K times — exercises branch predictor
    Stream,     // strided addresses, monotonic PC — exercises +1 prefetcher
    Random,     // uniform random PCs and addresses
};

struct GenParams {
    std::size_t   records       = 0;
    Pattern       pattern       = Pattern::Sequential;
    double        branch_rate   = 0.15;
    double        load_rate     = 0.30;
    double        store_rate    = 0.10;
    std::uint64_t seed          = 0xC0DEF00DULL;

    std::uint64_t pc_base       = 0x0040'0000ULL;
    std::uint64_t pc_stride     = 4;
    std::uint64_t addr_base     = 0x1000'0000ULL;
    std::uint64_t addr_stride   = 64;
    std::uint64_t loop_size     = 64;
};

void generate_records(const GenParams& p, trace::Writer& w);

std::optional<Pattern> parse_pattern(std::string_view s);
std::string_view       pattern_name(Pattern p);

} // namespace comparch::gen_trace
