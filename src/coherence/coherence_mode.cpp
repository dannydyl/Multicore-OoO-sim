// Driver for `comparch-sim --mode coherence`. Validates inputs, prints
// the project3-compatible banner, builds an N-CPU + 1-directory ring
// network with the chosen coherence protocol, and runs the per-cycle
// tick / tock loop until every CPU drains its trace.
//
// Output format is pinned against project3's printconf() + dump_stats()
// so the proj3 regression test can diff captured stdout against the
// reference outputs verbatim.

#include "comparch/coherence/coherence_mode.hpp"

#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/factory.hpp"
#include "comparch/coherence/network.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/log.hpp"
#include "comparch/trace.hpp"

#include <filesystem>
#include <iostream>

namespace comparch::coherence {

namespace {

// Hard cap shared with project3 (Simulator::run, simulator/sim.cpp:27).
// If the watchdog fires we still print stats with "Simulation terminated"
// instead of "Simulation complete".
constexpr Timestamp kMaxCycles = 500000;

Settings settings_from_cfg(const SimConfig& cfg) {
    Settings s;
    s.protocol        = parse_protocol(cfg.coherence.protocol);
    s.num_procs       = static_cast<NodeId>(cfg.cores);
    s.mem_latency     = static_cast<std::size_t>(cfg.memory.latency);
    s.block_size_log2 = static_cast<std::size_t>(cfg.interconnect.block_size_log2);
    s.link_width_log2 = static_cast<std::size_t>(cfg.interconnect.link_width_log2);
    finalize_settings(s);
    return s;
}

void print_completion(std::ostream& os, bool completed,
                      const CoherenceStats& stats) {
    if (completed) os << "Simulation complete\n";
    else           os << "Simulation terminated\n";
    os << "Cycles: " << stats.cycles << '\n';
    // Reuse the rest of the project3-compatible counter block.
    CoherenceStats body = stats;
    body.cycles = 0;          // print_stats prints its own Cycles header.
    // We can't actually reuse print_stats because it prints "Simulation
    // complete\n" and "Cycles:" itself — emit the seven counter lines
    // directly here, mirroring print_stats's setw layout.
    auto line = [&](const char* label, std::uint64_t v, const char* suf) {
        os << label;
        os.width(8); os << v; os << ' ' << suf << '\n';
    };
    line("Cache Misses:     ", stats.cache_misses,    "misses");
    line("Cache Accesses:   ", stats.cache_accesses,  "accesses");
    line("Silent Upgrades:  ", stats.silent_upgrades, "upgrades");
    line("$-to-$ Transfers: ", stats.c2c_transfers,   "transfers");
    line("Memory Reads:     ", stats.memory_reads,    "reads");
    line("Memory Writes:    ", stats.memory_writes,   "writes");
}

} // namespace

int run_coherence_mode(const SimConfig& cfg, const CliArgs& cli) {
    if (cli.trace) {
        LOG_ERROR("--mode coherence is per-core; pass --trace-dir DIR (with "
                  "p<i>.trace files), not --trace");
        return 1;
    }
    if (!cli.trace_dir) {
        LOG_ERROR("--mode coherence requires --trace-dir DIR");
        return 1;
    }
    if (cfg.interconnect.topology != "ring") {
        LOG_ERROR("interconnect.topology=" << cfg.interconnect.topology
                  << " is not supported in Phase 5A "
                     "(only 'ring'; 'xbar' is deferred)");
        return 2;
    }
    if (cfg.cores <= 0) {
        LOG_ERROR("cores must be positive, got " << cfg.cores);
        return 2;
    }

    for (int i = 0; i < cfg.cores; ++i) {
        const auto p = *cli.trace_dir / ("p" + std::to_string(i) + ".trace");
        if (!std::filesystem::exists(p)) {
            throw trace::TraceError("missing per-core trace: " + p.string());
        }
    }

    const auto settings = settings_from_cfg(cfg);
    print_banner(std::cout, cfg, cli.trace_dir->string());
    std::cout << "Starting simulation\n";

    CoherenceStats stats;
    Network net(settings, stats, *cli.trace_dir,
                make_agent_factory(settings.protocol));

    Timestamp clock = 0;
    bool completed  = false;
    while (true) {
        if (net.is_done()) { completed = true; break; }
        net.tick(clock);
        net.tock();
        ++clock;
        if (clock >= kMaxCycles) break;
    }

    stats.cycles = clock;
    print_completion(std::cout, completed, stats);
    return completed ? 0 : 5;
}

} // namespace comparch::coherence
