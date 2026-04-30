// Default-mode driver: N OoO cores, each with private L1+L2, ring-
// connected through the Phase 5A coherence subsystem.
//
// Per-cycle ordering:
//   1. Tick every OoO core (which internally ticks its own L1).
//   2. Tick the Network (CoherenceAdapter -> coherence::Cache ->
//      DirectoryController -> ring movement).
//   3. Tock the Network.
//
// Termination: every core reports done AND the Network has no in-flight
// messages. A global cycle cap (kGlobalCap) catches deadlock-shaped
// hangs that the per-core OoO watchdog can't see (e.g. mutual stalls
// across the ring).

#include "comparch/full/full_mode.hpp"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/cache_mode.hpp"
#include "comparch/coherence/coherence_adapter.hpp"
#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/directory.hpp"
#include "comparch/coherence/factory.hpp"
#include "comparch/coherence/network.hpp"
#include "comparch/coherence/settings.hpp"
#include "comparch/log.hpp"
#include "comparch/ooo/core.hpp"
#include "comparch/predictor/predictor.hpp"
#include "comparch/trace.hpp"

namespace fs = std::filesystem;

namespace comparch::full {

namespace {

constexpr coherence::Timestamp kGlobalCap = 5'000'000;

// Per-core ownership pack. The Network holds non-owning pointers into
// these (CoherenceAdapter as CpuPort, adapter->coh_cache() as the
// per-node coherence::Cache).
struct CoreStack {
    std::unique_ptr<predictor::BranchPredictor>  pred;
    std::unique_ptr<cache::Cache>                l2;
    std::unique_ptr<cache::Cache>                l1;
    std::unique_ptr<coherence::CoherenceAdapter> adapter;
    std::unique_ptr<trace::Reader>               reader;
    std::unique_ptr<ooo::OooCore>                core;
};

ooo::OooConfig to_ooo_config(const SimConfig& cfg) {
    ooo::OooConfig occ;
    occ.fetch_width           = static_cast<std::size_t>(cfg.core.fetch_width);
    occ.rob_entries           = static_cast<std::size_t>(cfg.core.rob_entries);
    occ.schedq_entries_per_fu = static_cast<std::size_t>(cfg.core.schedq_entries_per_fu);
    occ.alu_fus               = static_cast<std::size_t>(cfg.core.alu_fus);
    occ.mul_fus               = static_cast<std::size_t>(cfg.core.mul_fus);
    occ.lsu_fus               = static_cast<std::size_t>(cfg.core.lsu_fus);
    // Bump the per-core deadlock threshold considerably for full mode —
    // a single load might wait 100s of cycles for a coherence round-trip,
    // and we don't want the per-core watchdog to false-trip during
    // contention. The global cap (kGlobalCap) is the real backstop.
    occ.deadlock_threshold_cycles = 1'000'000;
    return occ;
}

coherence::Settings to_settings(const SimConfig& cfg) {
    coherence::Settings s;
    s.protocol        = coherence::parse_protocol(cfg.coherence.protocol);
    s.num_procs       = static_cast<coherence::NodeId>(cfg.cores);
    s.mem_latency     = static_cast<std::size_t>(cfg.memory.latency);
    s.block_size_log2 = static_cast<std::size_t>(cfg.interconnect.block_size_log2);
    s.link_width_log2 = static_cast<std::size_t>(cfg.interconnect.link_width_log2);
    coherence::finalize_settings(s);
    return s;
}

void print_stats(std::ostream& os,
                 const coherence::CoherenceStats& cs,
                 const std::vector<std::unique_ptr<CoreStack>>& stacks,
                 coherence::Timestamp clock,
                 bool completed,
                 const std::string& proto_label) {
    os << "==== full mode (" << stacks.size() << " cores, "
       << proto_label << ") ====\n";

    for (std::size_t i = 0; i < stacks.size(); ++i) {
        const auto& s = stacks[i]->core->stats();
        const double ipc =
            (s.cycles == 0)
                ? 0.0
                : static_cast<double>(s.instructions_retired) /
                  static_cast<double>(s.cycles);
        const double mpki =
            (s.instructions_retired == 0)
                ? 0.0
                : 1000.0 *
                  static_cast<double>(s.branch_mispredictions) /
                  static_cast<double>(s.instructions_retired);
        os << "  core " << i
           << ": cycles=" << s.cycles
           << "  retired=" << s.instructions_retired
           << "  ipc="  << std::fixed << std::setprecision(3) << ipc
           << "  mpki=" << std::fixed << std::setprecision(2) << mpki
           << "  l1d_misses=" << stacks[i]->l1->stats().misses
           << "  l2_misses="  << stacks[i]->l2->stats().read_misses
           << '\n';
    }

    os << (completed ? "Simulation complete\n" : "Simulation terminated\n");
    os << "Cycles: " << clock << '\n';

    auto line = [&](const char* label, std::uint64_t v, const char* suf) {
        os << label;
        os.width(8); os << v; os << ' ' << suf << '\n';
    };
    line("Cache Misses:     ", cs.cache_misses,    "misses");
    line("Cache Accesses:   ", cs.cache_accesses,  "accesses");
    line("Silent Upgrades:  ", cs.silent_upgrades, "upgrades");
    line("$-to-$ Transfers: ", cs.c2c_transfers,   "transfers");
    line("Memory Reads:     ", cs.memory_reads,    "reads");
    line("Memory Writes:    ", cs.memory_writes,   "writes");
}

} // namespace

int run_full_mode(const SimConfig& cfg, const CliArgs& cli) {
    if (cli.trace) {
        LOG_ERROR("default mode is per-core; pass --trace-dir DIR (with "
                  "p<i>.champsimtrace files), not --trace");
        return 1;
    }
    if (!cli.trace_dir) {
        LOG_ERROR("default mode requires --trace-dir DIR");
        return 1;
    }
    if (cfg.interconnect.topology != "ring") {
        LOG_ERROR("interconnect.topology=" << cfg.interconnect.topology
                  << " is not supported (only 'ring'; 'xbar' deferred)");
        return 2;
    }
    if (cfg.cores <= 0) {
        LOG_ERROR("cores must be positive, got " << cfg.cores);
        return 2;
    }

    std::vector<fs::path> trace_paths;
    trace_paths.reserve(cfg.cores);
    for (int i = 0; i < cfg.cores; ++i) {
        const auto p = *cli.trace_dir /
                       ("p" + std::to_string(i) + ".champsimtrace");
        if (!fs::exists(p)) {
            throw trace::TraceError("missing per-core trace: " + p.string());
        }
        trace_paths.push_back(p);
    }

    const auto settings = to_settings(cfg);
    coherence::CoherenceStats stats;

    std::vector<std::unique_ptr<CoreStack>> stack_owners;
    stack_owners.reserve(cfg.cores);

    for (int i = 0; i < cfg.cores; ++i) {
        auto cs = std::make_unique<CoreStack>();
        cs->pred = predictor::make(cfg.core.predictor);

        // L2 first, no sink yet (chicken-and-egg with the adapter).
        auto l2_cfg = cache::to_cache_config(cfg.l2);
        cs->l2 = std::make_unique<cache::Cache>(std::move(l2_cfg),
                                                "L2#" + std::to_string(i));

        // L1 next, points at L2 as its next_level.
        auto l1_cfg = cache::to_cache_config(cfg.l1);
        l1_cfg.next_level = cs->l2.get();
        cs->l1 = std::make_unique<cache::Cache>(std::move(l1_cfg),
                                                "L1#" + std::to_string(i));
        cs->l2->set_peer_above(cs->l1.get());

        cs->adapter = std::make_unique<coherence::CoherenceAdapter>(
            i, settings, stats,
            coherence::make_agent_factory(settings.protocol),
            *cs->l1, *cs->l2);

        // Splice the adapter behind L2 — now that we have a stable
        // adapter pointer, the L2 can call into it for misses/evictions.
        cs->l2->set_coherence_sink(cs->adapter.get());

        cs->reader = std::make_unique<trace::Reader>(
            trace_paths[i], trace::Variant::Standard);
        cs->core = std::make_unique<ooo::OooCore>(
            to_ooo_config(cfg), *cs->pred, *cs->l1, *cs->reader);

        stack_owners.push_back(std::move(cs));
    }

    std::vector<coherence::Network::CpuNode> cpu_nodes;
    cpu_nodes.reserve(cfg.cores);
    for (auto& cs : stack_owners) {
        cpu_nodes.push_back({cs->adapter.get(), cs->adapter->coh_cache()});
    }

    auto dir = std::make_unique<coherence::DirectoryController>(
        settings.num_procs, settings, stats);

    coherence::Network net(settings, stats, std::move(cpu_nodes),
                           std::move(dir));

    LOG_INFO("full mode: " << cfg.cores << " cores, protocol="
             << coherence::protocol_label(settings.protocol));

    coherence::Timestamp clock = 0;
    bool completed = false;
    while (true) {
        bool any_core_running = false;
        for (auto& cs : stack_owners) {
            if (cs->core->tick()) any_core_running = true;
        }
        if (!any_core_running && net.is_done()) { completed = true; break; }
        net.tick(clock);
        net.tock();
        ++clock;
        if (clock >= kGlobalCap) {
            LOG_ERROR("full mode: global cycle cap reached at " << clock
                      << " — possible coherence deadlock");
            break;
        }
    }

    print_stats(std::cout, stats, stack_owners, clock, completed,
                coherence::protocol_label(settings.protocol));
    return completed ? 0 : 5;
}

} // namespace comparch::full
