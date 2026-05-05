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

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "comparch/cli.hpp"

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

// Backstop against deadlock-shaped runs. 5M was too aggressive for
// homogeneous-shared synthetic workloads; 50M is a pragmatic compromise
// that catches genuine deadlocks without dragging tail-latency runs for
// 30 min. A 500M-cap diagnostic (see report_doc/11-validation-bugs.md
// "Empirical disambiguation") confirmed 42 of 44 cap-hit cases at 50M
// are slow-but-progressing and complete within 500M, plus 2 MI tail
// outliers that don't even fit in 30 min wallclock. The "real" fix is
// either heterogeneous traces or a finer-grained no-progress watchdog;
// this constant stays at 50M for routine harness use.
constexpr coherence::Timestamp kGlobalCap = 50'000'000;

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

// Reporter -------------------------------------------------------------
//
// `write_rpt` builds the human-readable report by calling section helpers
// in order. Adding a new section later (e.g. a future scheduling phase)
// is one new helper plus one call inside `write_rpt` — no other section
// needs to change.

struct ReportContext {
    const SimConfig&                                  cfg;
    const CliArgs&                                    cli;
    const coherence::CoherenceStats&                  cs;
    const std::vector<std::unique_ptr<CoreStack>>&    stacks;
    coherence::Timestamp                              clock;
    bool                                              completed;
    std::string                                       proto_label;   // "MESI_PRO"
};

// Per-core derived metrics, kept in one place so both .rpt and .csv use
// identical numbers.
//
// Cache-policy note: CacheStats has two parallel hit/miss vocabularies —
// `hits`/`misses` for WBWA caches and `read_hits`/`read_misses` for WTWNA.
// Each path zeroes the other's counters. We sum them so the metrics work
// for either configuration; `accesses` is universal (incremented on every
// access() entry regardless of policy).
struct CoreMetrics {
    std::uint64_t l1_hits, l1_misses;
    std::uint64_t l2_accesses, l2_hits, l2_misses;
    double        l1_miss_rate;
    double        l2_miss_rate;
    double        l1_aat;
    double        l2_aat;
    double        ipc;
    double        cpi;
    double        mpki;
};

CoreMetrics compute_metrics(const ooo::OooStats& s,
                            const cache::CacheStats& l1s,
                            const cache::CacheStats& l2s,
                            const SimConfig& cfg) {
    CoreMetrics m;
    m.l1_hits     = l1s.hits + l1s.read_hits;
    m.l1_misses   = l1s.misses + l1s.read_misses;
    m.l2_accesses = l2s.accesses;
    m.l2_hits     = l2s.hits + l2s.read_hits;
    m.l2_misses   = l2s.misses + l2s.read_misses;
    m.l1_miss_rate = l1s.accesses
        ? static_cast<double>(m.l1_misses) / static_cast<double>(l1s.accesses)
        : 0.0;
    m.l2_miss_rate = m.l2_accesses
        ? static_cast<double>(m.l2_misses) / static_cast<double>(m.l2_accesses)
        : 0.0;
    m.l2_aat = static_cast<double>(cfg.l2.hit_latency) +
               m.l2_miss_rate * static_cast<double>(cfg.memory.latency);
    m.l1_aat = static_cast<double>(cfg.l1.hit_latency) +
               m.l1_miss_rate * m.l2_aat;
    m.ipc = s.cycles
        ? static_cast<double>(s.instructions_retired) / static_cast<double>(s.cycles)
        : 0.0;
    m.cpi = s.instructions_retired
        ? static_cast<double>(s.cycles) / static_cast<double>(s.instructions_retired)
        : 0.0;
    m.mpki = s.instructions_retired
        ? 1000.0 * static_cast<double>(s.branch_mispredictions) /
                   static_cast<double>(s.instructions_retired)
        : 0.0;
    return m;
}

// Lowercase + strip "_pro" suffix so MESI_PRO -> mesi for folder names.
std::string proto_short(const std::string& label) {
    std::string out = label;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    const std::string suf = "_pro";
    if (out.size() > suf.size() &&
        out.compare(out.size() - suf.size(), suf.size(), suf) == 0) {
        out.erase(out.size() - suf.size());
    }
    return out;
}

// Aligned key:value line (column 28 split keeps numbers in a tidy column).
void kv(std::ostream& os, const char* indent, const std::string& key,
        const std::string& val) {
    constexpr std::size_t kKeyCol = 26;
    os << indent << std::left << std::setw(kKeyCol) << key
       << ": " << val << '\n';
}

template <typename T>
std::string str(const T& v) {
    std::ostringstream ss;
    ss << v;
    return ss.str();
}
std::string fix(double v, int prec) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}

void write_separator(std::ostream& os, char fill, const char* title = nullptr) {
    constexpr int kWidth = 80;
    os << std::string(kWidth, fill) << '\n';
    if (title) {
        os << ' ' << title << '\n';
        os << std::string(kWidth, fill) << '\n';
    }
}

void write_header(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "Multicore OoO Simulator -- Run Report");
    kv(os, "", "Trace",        ctx.cli.trace_dir->string());
    kv(os, "", "Cores",        str(ctx.cfg.cores));
    kv(os, "", "Protocol",     ctx.proto_label);
    kv(os, "", "Status",       ctx.completed ? "Simulation complete"
                                             : "Simulation terminated");
    kv(os, "", "Total cycles", str(ctx.clock));
    if (ctx.cli.tag) {
        kv(os, "", "Tag",      *ctx.cli.tag);
    }
    os << '\n';
}

void write_config(std::ostream& os, const ReportContext& ctx) {
    const auto& cfg = ctx.cfg;
    write_separator(os, '-', "Configuration parameters");

    os << "Core (per core, identical)\n";
    kv(os, "  ", "fetch_width",           str(cfg.core.fetch_width));
    kv(os, "  ", "rob_entries",           str(cfg.core.rob_entries));
    kv(os, "  ", "schedq_entries_per_fu", str(cfg.core.schedq_entries_per_fu));
    kv(os, "  ", "ALU / MUL / LSU FUs",
       str(cfg.core.alu_fus) + " / " + str(cfg.core.mul_fus) + " / " +
       str(cfg.core.lsu_fus));
    kv(os, "  ", "pipeline stages (A/M/L)",
       str(cfg.core.alu_stages) + " / " + str(cfg.core.mul_stages) + " / " +
       str(cfg.core.lsu_stages));
    kv(os, "  ", "branch predictor",
       cfg.core.predictor.type +
       " (H=" + str(cfg.core.predictor.history_bits) +
       ", P=" + str(cfg.core.predictor.pattern_bits) + ")");
    os << '\n';

    auto write_cache = [&](const char* name, const CacheLevelConfig& c) {
        os << name << " (per core, private)\n";
        kv(os, "  ", "size",          str(c.size_kb) + " KB");
        kv(os, "  ", "block size",    str(c.block_size) + " B");
        kv(os, "  ", "associativity", str(c.assoc) + "-way");
        kv(os, "  ", "replacement",   c.replacement);
        kv(os, "  ", "write policy",  c.write_policy);
        kv(os, "  ", "hit latency",   str(c.hit_latency) + " cycles");
        kv(os, "  ", "prefetcher",    c.prefetcher);
        kv(os, "  ", "MSHR entries",  str(c.mshr_entries));
        os << '\n';
    };
    write_cache("L1 cache", cfg.l1);
    write_cache("L2 cache", cfg.l2);

    os << "Memory\n";
    kv(os, "  ", "latency",    str(cfg.memory.latency) + " cycles");
    kv(os, "  ", "block size", str(cfg.memory.block_size) + " B");
    os << '\n';

    os << "Interconnect\n";
    kv(os, "  ", "topology",     cfg.interconnect.topology);
    kv(os, "  ", "link latency", str(cfg.interconnect.link_latency));
    kv(os, "  ", "link width",
       std::string("2^") + str(cfg.interconnect.link_width_log2) +
       " = " + str(1 << cfg.interconnect.link_width_log2) + " B");
    kv(os, "  ", "block size",
       std::string("2^") + str(cfg.interconnect.block_size_log2) +
       " = " + str(1 << cfg.interconnect.block_size_log2) + " B");
    os << '\n';
}

void write_per_core(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "Per-core results");

    for (std::size_t i = 0; i < ctx.stacks.size(); ++i) {
        const auto& s   = ctx.stacks[i]->core->stats();
        const auto& l1s = ctx.stacks[i]->l1->stats();
        const auto& l2s = ctx.stacks[i]->l2->stats();
        const auto m    = compute_metrics(s, l1s, l2s, ctx.cfg);

        os << "[ Core " << i << " ]\n";
        os << "  Pipeline\n";
        kv(os, "    ", "cycles",                str(s.cycles));
        kv(os, "    ", "instructions retired",  str(s.instructions_retired));
        kv(os, "    ", "instructions fetched",  str(s.instructions_fetched));
        kv(os, "    ", "IPC",                   fix(m.ipc, 3));
        kv(os, "    ", "CPI",                   fix(m.cpi, 3));
        kv(os, "    ", "branch mispredictions", str(s.branch_mispredictions));
        kv(os, "    ", "MPKI",                  fix(m.mpki, 2));

        os << "  L1 cache\n";
        kv(os, "    ", "accesses",  str(l1s.accesses));
        kv(os, "    ", "hits",      str(m.l1_hits));
        kv(os, "    ", "misses",    str(m.l1_misses));
        kv(os, "    ", "miss rate", fix(m.l1_miss_rate, 3));
        kv(os, "    ", "AAT",       fix(m.l1_aat, 2) + " cycles");

        os << "  L2 cache\n";
        kv(os, "    ", "accesses",  str(m.l2_accesses));
        kv(os, "    ", "hits",      str(m.l2_hits));
        kv(os, "    ", "misses",    str(m.l2_misses));
        kv(os, "    ", "miss rate", fix(m.l2_miss_rate, 3));
        kv(os, "    ", "AAT",       fix(m.l2_aat, 2) + " cycles");

        if (i + 1 < ctx.stacks.size()) os << '\n';
    }
    os << '\n';
}

void write_system(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "System-wide (coherence + memory)");
    const auto& cs = ctx.cs;
    kv(os, "", "Cache accesses",            str(cs.cache_accesses));
    kv(os, "", "Cache misses",              str(cs.cache_misses));
    kv(os, "", "Silent upgrades",           str(cs.silent_upgrades));
    kv(os, "", "Cache-to-cache transfers",  str(cs.c2c_transfers));
    kv(os, "", "Memory reads",              str(cs.memory_reads));
    kv(os, "", "Memory writes",             str(cs.memory_writes));
}

void write_rpt(std::ostream& os, const ReportContext& ctx) {
    write_header(os, ctx);
    write_config(os, ctx);
    write_per_core(os, ctx);
    write_system(os, ctx);
}

void write_csv(std::ostream& os, const ReportContext& ctx) {
    os << "core,cycles,instructions_retired,instructions_fetched,"
          "ipc,cpi,branch_mispredictions,mpki,"
          "l1_accesses,l1_hits,l1_misses,l1_miss_rate,l1_aat,"
          "l2_accesses,l2_hits,l2_misses,l2_miss_rate,l2_aat\n";
    os << std::fixed;
    for (std::size_t i = 0; i < ctx.stacks.size(); ++i) {
        const auto& s   = ctx.stacks[i]->core->stats();
        const auto& l1s = ctx.stacks[i]->l1->stats();
        const auto& l2s = ctx.stacks[i]->l2->stats();
        const auto m    = compute_metrics(s, l1s, l2s, ctx.cfg);
        os << i
           << ',' << s.cycles
           << ',' << s.instructions_retired
           << ',' << s.instructions_fetched
           << ',' << std::setprecision(5) << m.ipc
           << ',' << std::setprecision(5) << m.cpi
           << ',' << s.branch_mispredictions
           << ',' << std::setprecision(5) << m.mpki
           << ',' << l1s.accesses
           << ',' << m.l1_hits
           << ',' << m.l1_misses
           << ',' << std::setprecision(5) << m.l1_miss_rate
           << ',' << std::setprecision(5) << m.l1_aat
           << ',' << m.l2_accesses
           << ',' << m.l2_hits
           << ',' << m.l2_misses
           << ',' << std::setprecision(5) << m.l2_miss_rate
           << ',' << std::setprecision(5) << m.l2_aat
           << '\n';
    }
}

fs::path build_run_dir(const ReportContext& ctx) {
    std::string name = ctx.cli.trace_dir->filename().string() + "_" +
                       proto_short(ctx.proto_label) + "_c" +
                       str(ctx.cfg.cores);
    if (ctx.cli.tag && !ctx.cli.tag->empty()) {
        name += "_" + *ctx.cli.tag;
    }
    return fs::path("report") / name;
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

    ReportContext ctx{
        cfg, cli, stats, stack_owners, clock, completed,
        coherence::protocol_label(settings.protocol)};

    write_rpt(std::cout, ctx);

    const fs::path run_dir = build_run_dir(ctx);
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    if (ec) {
        LOG_WARN("could not create " << run_dir << ": " << ec.message()
                 << "; skipping report files");
    } else {
        if (std::ofstream rpt(run_dir / "report.rpt"); rpt) {
            write_rpt(rpt, ctx);
        } else {
            LOG_WARN("could not open " << (run_dir / "report.rpt")
                     << " for writing");
        }
        if (std::ofstream csv(run_dir / "report.csv"); csv) {
            write_csv(csv, ctx);
        } else {
            LOG_WARN("could not open " << (run_dir / "report.csv")
                     << " for writing");
        }
        LOG_INFO("wrote report to " << run_dir);
    }

    return completed ? 0 : 5;
}

} // namespace comparch::full
