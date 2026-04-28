#pragma once

#include <cstddef>
#include <iosfwd>
#include <string>
#include <string_view>

#include "comparch/trace.hpp"

// One-shot converter from project2's 11-field text trace format to a ChampSim
// binary trace, used to produce regression fixtures that cross-validate our
// predictor implementations against project2's reference numbers.
//
// project2 line shape (whitespace-separated):
//
//   pc opcode dest src1 src2 load_store_addr br_taken br_target \
//       icache_hit dcache_hit dyn_instruction_count
//
// Numeric formats: pc / load_store_addr / br_target are hex without 0x prefix;
// opcode is decimal 2..6 mapping to ADD/MUL/LOAD/STORE/BRANCH; dest/src* are
// signed int8 (-1 means unused); icache_hit / dcache_hit are decimal flags;
// dyn_instruction_count is decimal u64.
//
// We translate one line to one ChampSim Record: branches set is_branch +
// branch_taken; loads put load_store_addr in source_memory[0]; stores put it
// in destination_memory[0]; ALU/MUL ops touch no memory. Cache-hit fields
// from project2 are dropped — ChampSim's format has no slot for them, and the
// new sim computes them from a real cache model anyway.

namespace comparch::proj2 {

// Result of parsing one trace line. `ok` is false on malformed input; in that
// case `error` holds a short diagnostic.
struct ParseResult {
    bool         ok = false;
    std::string  error;
};

// Parse one project2 text-format trace line into a ChampSim Record. Returns
// {ok=false, error=...} on malformed input. Empty / whitespace-only lines
// are reported as ok=false with an explanatory message so callers can decide
// whether to skip or stop.
ParseResult parse_line(std::string_view line, trace::Record& out);

// Stream the conversion: read project2-format lines from `in`, emit one
// ChampSim record each via `writer`. Returns the number of records written.
// Throws comparch::trace::TraceError on parse error, with the line number
// included in the message.
std::size_t convert(std::istream& in, trace::Writer& writer);

} // namespace comparch::proj2
