// Project3-compatible banner and stats printer. Format strings are pinned
// against the legacy `printf` calls (simulator/main.cpp:82-90 and
// simulator/sim.cpp:54-60); changes here must be diffed against
// project3_v1.1.0/ref_outs/*.out before commit.

#include "comparch/coherence/coherence_stats.hpp"

#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <string>

namespace comparch::coherence {

std::string proto_label(const std::string& protocol) {
    if (protocol == "mi")     return "MI_PRO";
    if (protocol == "msi")    return "MSI_PRO";
    if (protocol == "mesi")   return "MESI_PRO";
    if (protocol == "mosi")   return "MOSI_PRO";
    if (protocol == "moesif") return "MOESIF_PRO";
    throw ConfigError("unknown coherence protocol: '" + protocol + "'");
}

void print_banner(std::ostream& os,
                  const SimConfig& cfg,
                  const std::string& trace_dir_label) {
    // Asymmetry preserved from project3: Link Width prints (1 << link_width)
    // bytes, but Block Size prints the *exponent* (e.g. 6 for 64-byte lines).
    const auto link_width_bytes = 1ULL << cfg.interconnect.link_width_log2;

    os << "Selected Configuration:\n"
       << "\tProtocol: "         << proto_label(cfg.coherence.protocol) << '\n'
       << "\tTrace Directory: "  << trace_dir_label << '\n'
       << "\tNum Procs: "        << cfg.cores << '\n'
       << "\tCPU TYPE: FICI_CPU\n"
       << "\tNetwork Topology: RING_TOP\n"
       << "\tLink Width: "       << link_width_bytes << " bytes\n"
       << "\tMemory Latency: "   << cfg.memory.latency << '\n'
       << "\tBlock Size: "       << cfg.interconnect.block_size_log2 << '\n';
}

namespace {

// "<label>%8d <suffix>\n" with the same column alignment as project3's
// printf("Label:     %8" PRIu64 " suffix\n").
void print_line(std::ostream& os, const char* label,
                std::uint64_t value, const char* suffix) {
    os << label << std::setw(8) << value << ' ' << suffix << '\n';
}

} // namespace

void print_stats(std::ostream& os, const CoherenceStats& s) {
    os << "Simulation complete\n";
    os << "Cycles: " << s.cycles << '\n';
    print_line(os, "Cache Misses:     ",  s.cache_misses,    "misses");
    print_line(os, "Cache Accesses:   ",  s.cache_accesses,  "accesses");
    print_line(os, "Silent Upgrades:  ",  s.silent_upgrades, "upgrades");
    print_line(os, "$-to-$ Transfers: ",  s.c2c_transfers,   "transfers");
    print_line(os, "Memory Reads:     ",  s.memory_reads,    "reads");
    print_line(os, "Memory Writes:    ",  s.memory_writes,   "writes");
}

} // namespace comparch::coherence
