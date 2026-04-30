#pragma once

// Project3-compatible stat collection and printing.
//
// The format printed by `print_banner` and `print_stats` must match
// project3's `printconf()` (simulator/main.cpp:81) and `Simulator::dump_stats`
// (simulator/sim.cpp:54-60) byte-for-byte; the regression test diffs captured
// stdout against `<P>_core_<N>.out` fixtures, so any format drift breaks parity.

#include <cstdint>
#include <iosfwd>
#include <string>

#include "comparch/config.hpp"

namespace comparch::coherence {

struct CoherenceStats {
    std::uint64_t cycles         = 0;
    std::uint64_t cache_misses   = 0;
    std::uint64_t cache_accesses = 0;
    std::uint64_t silent_upgrades = 0;
    std::uint64_t c2c_transfers  = 0;
    std::uint64_t memory_reads   = 0;
    std::uint64_t memory_writes  = 0;
};

// Returns the legacy proto_str entry for `protocol` ("MSI" -> "MSI_PRO").
// Throws ConfigError on an unrecognized name.
std::string proto_label(const std::string& protocol);

// Project3-compatible "Selected Configuration:" + per-line "\t<key>: <value>"
// block. Echoes `trace_dir_label` verbatim; pass the user-facing label
// (e.g. "traces/core_4"), not an absolute path.
void print_banner(std::ostream& os,
                  const SimConfig& cfg,
                  const std::string& trace_dir_label);

// "Simulation complete\n" + 7 stat lines in project3's exact field-width
// format. Caller decides whether to emit "Starting simulation\n" before
// running and "Simulation complete\n" via this helper after.
void print_stats(std::ostream& os, const CoherenceStats& s);

} // namespace comparch::coherence
