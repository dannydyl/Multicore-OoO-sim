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
#include "comparch/ooo/trace_logger.hpp"
#include "comparch/predictor/predictor.hpp"
#include "comparch/trace.hpp"

#include <cstdlib>   // getenv

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
constexpr coherence::Timestamp kGlobalCap = 100'000'000;

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
    occ.dispq_capacity        = static_cast<std::size_t>(cfg.core.dispq_capacity);
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

// Trim ASCII whitespace from both ends.
std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back())))  s.pop_back();
    return s;
}

// Resolve per-core trace paths from CLI input. Two input modes:
//   --trace-dir DIR : <DIR>/p<i>.champsimtrace, i in [0, cores).
//   --trace-list F  : one path per line; blank/'#' lines skipped; relative
//                     paths resolved against F's parent directory; entry
//                     count must equal cores.
// Caller has already verified exactly one of the two is set.
std::vector<fs::path> resolve_per_core_traces(const CliArgs& cli, int cores) {
    std::vector<fs::path> paths;
    paths.reserve(cores);

    if (cli.trace_dir) {
        for (int i = 0; i < cores; ++i) {
            const auto p = *cli.trace_dir /
                           ("p" + std::to_string(i) + ".champsimtrace");
            if (!fs::exists(p)) {
                throw trace::TraceError("missing per-core trace: " + p.string());
            }
            paths.push_back(p);
        }
        return paths;
    }

    const auto& manifest = *cli.trace_list;
    const auto manifest_dir = manifest.parent_path();
    std::ifstream in(manifest);
    if (!in) {
        throw trace::TraceError("could not open --trace-list file: " +
                                manifest.string());
    }
    std::string line;
    while (std::getline(in, line)) {
        // Strip inline comment first ("path # note") then trim. This
        // matches the convention used by run_sweep.py and gen_synth.py
        // and is a no-op for whole-line comments (they shrink to "").
        if (auto hash = line.find('#'); hash != std::string::npos) {
            line.erase(hash);
        }
        line = trim(line);
        if (line.empty()) continue;
        fs::path p = line;
        if (p.is_relative()) p = manifest_dir / p;
        if (!fs::exists(p)) {
            throw trace::TraceError("missing trace from --trace-list: " +
                                    p.string());
        }
        paths.push_back(std::move(p));
    }
    if (static_cast<int>(paths.size()) != cores) {
        throw trace::TraceError(
            "--trace-list has " + std::to_string(paths.size()) +
            " entries but cores=" + std::to_string(cores));
    }
    return paths;
}

// Human-readable label for whichever trace input the user supplied.
// Used in the report header and the run-directory name.
std::string trace_label(const CliArgs& cli) {
    if (cli.trace_dir)  return cli.trace_dir->string();
    if (cli.trace_list) return cli.trace_list->string();
    return {};
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
    // Cache totals (WBWA + WTWNA reconciled).
    std::uint64_t l1_hits, l1_misses;
    std::uint64_t l2_accesses, l2_hits, l2_misses;

    // Cache rates.
    double        l1_miss_rate;     // L1 misses / L1 accesses
    double        l1_hit_rate;      // L1 hits   / L1 accesses
    double        l2_miss_rate;     // local: L2 misses / L2 accesses
    double        l2_hit_rate;      // 1 - l2_miss_rate
    double        l2_global_miss;   // L2 misses / L1 accesses (global miss rate)

    // Average Memory Access Time (Hennessy & Patterson Ch. 2).
    //   AMAT_L2 = hit_lat_L2 + miss_rate_L2 * mem_lat
    //   AMAT_L1 = hit_lat_L1 + miss_rate_L1 * AMAT_L2
    double        l1_aat;
    double        l2_aat;

    // Pipeline throughput.
    double        ipc;
    double        cpi;

    // Branch metrics.
    double        branch_accuracy;          // (br - mispred) / br
    double        branch_mispred_rate;      // mispred / br
    double        mpki;                     // mispred * 1000 / retired
    double        branches_per_kinst;       // br * 1000 / retired

    // Cache pressure (per-kilo-instruction). APKI = accesses-per-kilo-inst.
    double        l1_apki;
    double        l1_mpki;                  // L1 misses * 1000 / retired
    double        l2_mpki;                  // L2 misses * 1000 / retired

    // Pipeline-resource occupancy. Average / max sizes are reported by
    // OooStats; the percentages below normalize against capacity.
    double        rob_occupancy_pct;
    double        schedq_occupancy_pct;
    double        dispq_occupancy_pct;

    // Stall fractions.
    double        no_fire_pct;              // cycles with 0 issues
    double        rob_full_pct;             // dispatch-blocked-by-rob cycles
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
    m.l1_hit_rate = 1.0 - m.l1_miss_rate;
    m.l2_miss_rate = m.l2_accesses
        ? static_cast<double>(m.l2_misses) / static_cast<double>(m.l2_accesses)
        : 0.0;
    m.l2_hit_rate = 1.0 - m.l2_miss_rate;
    m.l2_global_miss = l1s.accesses
        ? static_cast<double>(m.l2_misses) / static_cast<double>(l1s.accesses)
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
    m.branches_per_kinst = s.instructions_retired
        ? 1000.0 * static_cast<double>(s.num_branch_instructions) /
                   static_cast<double>(s.instructions_retired)
        : 0.0;
    m.branch_accuracy = s.num_branch_instructions
        ? static_cast<double>(s.num_branch_instructions - s.branch_mispredictions) /
          static_cast<double>(s.num_branch_instructions)
        : 0.0;
    m.branch_mispred_rate = s.num_branch_instructions
        ? static_cast<double>(s.branch_mispredictions) /
          static_cast<double>(s.num_branch_instructions)
        : 0.0;
    m.l1_apki = s.instructions_retired
        ? 1000.0 * static_cast<double>(l1s.accesses) /
                   static_cast<double>(s.instructions_retired)
        : 0.0;
    m.l1_mpki = s.instructions_retired
        ? 1000.0 * static_cast<double>(m.l1_misses) /
                   static_cast<double>(s.instructions_retired)
        : 0.0;
    m.l2_mpki = s.instructions_retired
        ? 1000.0 * static_cast<double>(m.l2_misses) /
                   static_cast<double>(s.instructions_retired)
        : 0.0;
    // Resource occupancy. The OoO config knows the capacities.
    const double rob_cap    = static_cast<double>(cfg.core.rob_entries);
    const double schedq_cap = static_cast<double>(
        cfg.core.schedq_entries_per_fu *
        (cfg.core.alu_fus + cfg.core.mul_fus + cfg.core.lsu_fus));
    const double dispq_cap  = static_cast<double>(cfg.core.dispq_capacity);
    m.rob_occupancy_pct    = rob_cap    > 0 ? 100.0 * s.rob_avg()    / rob_cap    : 0.0;
    m.schedq_occupancy_pct = schedq_cap > 0 ? 100.0 * s.schedq_avg() / schedq_cap : 0.0;
    m.dispq_occupancy_pct  = dispq_cap  > 0 ? 100.0 * s.dispq_avg()  / dispq_cap  : 0.0;
    m.no_fire_pct = s.cycles
        ? 100.0 * static_cast<double>(s.no_fire_cycles) /
                  static_cast<double>(s.cycles)
        : 0.0;
    m.rob_full_pct = s.cycles
        ? 100.0 * static_cast<double>(s.rob_no_dispatch_cycles) /
                  static_cast<double>(s.cycles)
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
    kv(os, "", "Trace",        trace_label(ctx.cli));
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

// Concise per-core overview for report.rpt — one block per core, ~6
// lines each, matching the columns in report.csv.
void write_overview_per_core(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '-', "Per-core summary");
    for (std::size_t i = 0; i < ctx.stacks.size(); ++i) {
        const auto& s   = ctx.stacks[i]->core->stats();
        const auto& l1s = ctx.stacks[i]->l1->stats();
        const auto& l2s = ctx.stacks[i]->l2->stats();
        const auto m    = compute_metrics(s, l1s, l2s, ctx.cfg);

        os << "[ Core " << i << " ]\n";
        kv(os, "  ", "cycles",                str(s.cycles));
        kv(os, "  ", "instructions retired",  str(s.instructions_retired));
        kv(os, "  ", "IPC / CPI",             fix(m.ipc, 3) + " / " + fix(m.cpi, 3));
        kv(os, "  ", "L1 miss rate",   fix(m.l1_miss_rate, 4) +
                                       " (" + str(m.l1_misses) + "/" +
                                       str(l1s.accesses) + ")");
        kv(os, "  ", "L2 miss rate",   fix(m.l2_miss_rate, 4) +
                                       " (" + str(m.l2_misses) + "/" +
                                       str(m.l2_accesses) + ")");
        kv(os, "  ", "branch MPKI",    fix(m.mpki, 2));
        if (i + 1 < ctx.stacks.size()) os << '\n';
    }
    os << '\n';
}

// Aggregate banner for report.rpt: total cycles, total retired, average
// IPC across cores. Same numbers a sweeper plotter would use.
void write_aggregate(std::ostream& os, const ReportContext& ctx) {
    std::uint64_t total_retired = 0;
    std::uint64_t total_cycles  = 0;
    double        ipc_sum       = 0.0;
    std::uint64_t total_l1_acc  = 0;
    std::uint64_t total_l1_miss = 0;
    for (auto& cs : ctx.stacks) {
        const auto& s   = cs->core->stats();
        const auto& l1s = cs->l1->stats();
        total_retired += s.instructions_retired;
        total_cycles   = std::max(total_cycles, s.cycles);
        ipc_sum       += s.ipc();
        total_l1_acc  += l1s.accesses;
        total_l1_miss += l1s.misses + l1s.read_misses;
    }
    const double aggregate_ipc = static_cast<double>(total_retired) /
        static_cast<double>(std::max<std::uint64_t>(1, total_cycles));
    const double l1_miss_rate = total_l1_acc
        ? static_cast<double>(total_l1_miss) / static_cast<double>(total_l1_acc)
        : 0.0;

    write_separator(os, '-', "System aggregate");
    kv(os, "", "Total instr retired",   str(total_retired));
    kv(os, "", "Aggregate IPC",         fix(aggregate_ipc, 3));
    kv(os, "", "Per-core IPC (mean)",
       fix(ipc_sum / static_cast<double>(ctx.stacks.size()), 3));
    kv(os, "", "L1 system miss rate",   fix(l1_miss_rate, 4));
    os << '\n';
}

// Detailed per-core stats for stats.rpt. This is where most of the
// industry-style numbers live: AMAT, MPKI, branch accuracy, occupancy
// percentages, stall fractions, prefetcher accounting, etc.
void write_detailed_per_core(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "Per-core detailed statistics");

    for (std::size_t i = 0; i < ctx.stacks.size(); ++i) {
        const auto& s   = ctx.stacks[i]->core->stats();
        const auto& l1s = ctx.stacks[i]->l1->stats();
        const auto& l2s = ctx.stacks[i]->l2->stats();
        const auto m    = compute_metrics(s, l1s, l2s, ctx.cfg);

        os << "[ Core " << i << " ]\n";

        os << "  Pipeline throughput\n";
        kv(os, "    ", "cycles",                str(s.cycles));
        kv(os, "    ", "instructions fetched",  str(s.instructions_fetched));
        kv(os, "    ", "instructions retired",  str(s.instructions_retired));
        kv(os, "    ", "IPC",                   fix(m.ipc, 4));
        kv(os, "    ", "CPI",                   fix(m.cpi, 4));
        kv(os, "    ", "deadlock state",
           s.deadlocked ? ("DEADLOCKED@" + str(s.stall_cycles_at_abort))
                        : std::string("clean"));

        os << "  Branch predictor\n";
        kv(os, "    ", "branches",              str(s.num_branch_instructions));
        kv(os, "    ", "mispredictions",        str(s.branch_mispredictions));
        kv(os, "    ", "branch accuracy",       fix(100.0 * m.branch_accuracy, 2) + " %");
        kv(os, "    ", "mispredict rate",       fix(100.0 * m.branch_mispred_rate, 2) + " %");
        kv(os, "    ", "branches/kinst",        fix(m.branches_per_kinst, 2));
        kv(os, "    ", "MPKI (mispred)",        fix(m.mpki, 2));

        os << "  L1 cache (" << ctx.cfg.l1.size_kb << " KB / "
           << ctx.cfg.l1.assoc << "-way / " << ctx.cfg.l1.write_policy << ")\n";
        kv(os, "    ", "accesses",              str(l1s.accesses));
        kv(os, "    ", "reads / writes",
           str(l1s.reads) + " / " + str(l1s.writes));
        kv(os, "    ", "hits",                  str(m.l1_hits));
        kv(os, "    ", "misses",                str(m.l1_misses));
        kv(os, "    ", "hit rate",              fix(100.0 * m.l1_hit_rate, 3) + " %");
        kv(os, "    ", "miss rate",             fix(100.0 * m.l1_miss_rate, 3) + " %");
        kv(os, "    ", "APKI",                  fix(m.l1_apki, 2));
        kv(os, "    ", "MPKI",                  fix(m.l1_mpki, 2));
        kv(os, "    ", "AMAT",                  fix(m.l1_aat, 2) + " cycles");
        kv(os, "    ", "writebacks",            str(l1s.writebacks));
        kv(os, "    ", "coherence invals",      str(l1s.coherence_invals));
        if (l1s.prefetches_issued != 0) {
            kv(os, "    ", "prefetches issued",  str(l1s.prefetches_issued));
            kv(os, "    ", "prefetch hits",      str(l1s.prefetch_hits));
            kv(os, "    ", "prefetch misses",    str(l1s.prefetch_misses));
        }

        os << "  L2 cache (" << ctx.cfg.l2.size_kb << " KB / "
           << ctx.cfg.l2.assoc << "-way / " << ctx.cfg.l2.write_policy << ")\n";
        kv(os, "    ", "accesses",              str(m.l2_accesses));
        kv(os, "    ", "hits",                  str(m.l2_hits));
        kv(os, "    ", "misses",                str(m.l2_misses));
        kv(os, "    ", "local hit rate",        fix(100.0 * m.l2_hit_rate, 3) + " %");
        kv(os, "    ", "local miss rate",       fix(100.0 * m.l2_miss_rate, 3) + " %");
        kv(os, "    ", "global miss rate",      fix(100.0 * m.l2_global_miss, 3) + " %  (vs L1 accesses)");
        kv(os, "    ", "MPKI",                  fix(m.l2_mpki, 2));
        kv(os, "    ", "AMAT",                  fix(m.l2_aat, 2) + " cycles");
        kv(os, "    ", "writebacks",            str(l2s.writebacks));
        kv(os, "    ", "coherence invals",      str(l2s.coherence_invals));
        if (l2s.prefetches_issued != 0) {
            kv(os, "    ", "prefetches issued",  str(l2s.prefetches_issued));
            kv(os, "    ", "prefetch hits",      str(l2s.prefetch_hits));
            kv(os, "    ", "prefetch misses",    str(l2s.prefetch_misses));
        }

        os << "  Pipeline resources (avg / max / capacity / occupancy %)\n";
        kv(os, "    ", "ROB",
           fix(s.rob_avg(), 1) + " / " + str(s.rob_max) + " / " +
           str(ctx.cfg.core.rob_entries) + " / " +
           fix(m.rob_occupancy_pct, 1) + " %");
        const std::size_t schedq_cap = ctx.cfg.core.schedq_entries_per_fu *
            (ctx.cfg.core.alu_fus + ctx.cfg.core.mul_fus + ctx.cfg.core.lsu_fus);
        kv(os, "    ", "SchedQ",
           fix(s.schedq_avg(), 1) + " / " + str(s.schedq_max) + " / " +
           str(schedq_cap) + " / " +
           fix(m.schedq_occupancy_pct, 1) + " %");
        kv(os, "    ", "DispQ",
           fix(s.dispq_avg(), 1) + " / " + str(s.dispq_max) + " / " +
           str(ctx.cfg.core.dispq_capacity) + " / " +
           fix(m.dispq_occupancy_pct, 1) + " %");

        os << "  Stall accounting\n";
        kv(os, "    ", "no-fire cycles",
           str(s.no_fire_cycles) + " (" + fix(m.no_fire_pct, 2) + " %)");
        kv(os, "    ", "ROB-full dispatch stalls",
           str(s.rob_no_dispatch_cycles) + " (" + fix(m.rob_full_pct, 2) + " %)");

        if (i + 1 < ctx.stacks.size()) os << '\n';
    }
    os << '\n';

    os << "AMAT formula (Hennessy & Patterson Ch. 2):\n"
       << "    AMAT_L2 = hit_lat_L2 + miss_rate_L2 * mem_lat\n"
       << "    AMAT_L1 = hit_lat_L1 + miss_rate_L1 * AMAT_L2\n"
       << "  with hit_lat_L1 = " << ctx.cfg.l1.hit_latency
       << ", hit_lat_L2 = " << ctx.cfg.l2.hit_latency
       << ", mem_lat = " << ctx.cfg.memory.latency << " cycles.\n";
}

// Coherence / off-chip section, broken out to its own file so config
// sweeps that vary protocol can diff coherence.rpt without churning
// the rest. Numbers come from the system-wide CoherenceStats counter
// pack — there is no per-core split today.
void write_coherence(std::ostream& os, const ReportContext& ctx) {
    const auto& cs = ctx.cs;
    write_separator(os, '=', "Coherence + memory");
    kv(os, "", "Protocol",                  ctx.proto_label);
    kv(os, "", "Cache accesses (system)",   str(cs.cache_accesses));
    kv(os, "", "Cache misses (system)",     str(cs.cache_misses));
    kv(os, "", "Silent upgrades",           str(cs.silent_upgrades));
    kv(os, "", "Cache-to-cache transfers",  str(cs.c2c_transfers));
    kv(os, "", "Memory reads",              str(cs.memory_reads));
    kv(os, "", "Memory writes",             str(cs.memory_writes));

    // Derived ratios. Useful for comparing protocols at a glance:
    //   c2c_share  = how many misses were satisfied by a peer cache
    //                (intervention) vs. memory.
    //   wb_per_inv = how often invalidations turn into actual dirty
    //                evictions; tells you how chatty a write-heavy
    //                workload is on this protocol.
    if (cs.cache_misses != 0) {
        const double c2c_share = static_cast<double>(cs.c2c_transfers) /
                                 static_cast<double>(cs.cache_misses);
        kv(os, "", "C2C / miss",            fix(c2c_share, 4));
    }
    if (cs.cache_accesses != 0) {
        const double system_miss_rate = static_cast<double>(cs.cache_misses) /
                                        static_cast<double>(cs.cache_accesses);
        kv(os, "", "System miss rate",      fix(100.0 * system_miss_rate, 3) + " %");
    }

    // System-wide invalidation count (sum across L1+L2 of every core).
    std::uint64_t invs_total = 0;
    for (auto& cs2 : ctx.stacks) {
        invs_total += cs2->l1->stats().coherence_invals;
        invs_total += cs2->l2->stats().coherence_invals;
    }
    kv(os, "", "Coherence invalidations",   str(invs_total));
}

// stdout / report.rpt — the concise overview the user reads first.
void write_overview(std::ostream& os, const ReportContext& ctx) {
    write_header(os, ctx);
    write_aggregate(os, ctx);
    write_overview_per_core(os, ctx);
}

// stats.rpt — full pipeline + cache + branch breakdown.
void write_stats(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "Multicore OoO Simulator -- Detailed Stats");
    kv(os, "", "Trace",        trace_label(ctx.cli));
    kv(os, "", "Cores",        str(ctx.cfg.cores));
    kv(os, "", "Protocol",     ctx.proto_label);
    kv(os, "", "Total cycles", str(ctx.clock));
    if (ctx.cli.tag) kv(os, "", "Tag", *ctx.cli.tag);
    os << '\n';
    write_detailed_per_core(os, ctx);
}

// config.rpt — pure config dump. No simulation numbers.
void write_config_only(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "Multicore OoO Simulator -- Configuration");
    kv(os, "", "Trace",        trace_label(ctx.cli));
    kv(os, "", "Cores",        str(ctx.cfg.cores));
    kv(os, "", "Protocol",     ctx.proto_label);
    if (ctx.cli.tag) kv(os, "", "Tag", *ctx.cli.tag);
    os << '\n';
    write_config(os, ctx);
}

// coherence.rpt — protocol counters + derived ratios.
void write_coherence_file(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "Multicore OoO Simulator -- Coherence");
    kv(os, "", "Trace",        trace_label(ctx.cli));
    kv(os, "", "Cores",        str(ctx.cfg.cores));
    kv(os, "", "Total cycles", str(ctx.clock));
    if (ctx.cli.tag) kv(os, "", "Tag", *ctx.cli.tag);
    os << '\n';
    write_coherence(os, ctx);
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

// Pre-simulation path resolver — used to open log.rpt before tick()
// runs. Doesn't need a ReportContext; that doesn't exist yet at this
// point in the driver.
fs::path build_run_dir_pre(const SimConfig& cfg, const CliArgs& cli,
                           const std::string& proto_label) {
    std::string stem;
    if (cli.trace_dir)       stem = cli.trace_dir->filename().string();
    else if (cli.trace_list) stem = cli.trace_list->stem().string();
    std::string name = stem + "_" + proto_short(proto_label) + "_c" +
                       str(cfg.cores);
    if (cli.tag && !cli.tag->empty()) name += "_" + *cli.tag;
    return fs::path("report") / name;
}

// LOG=1 / LOG=true / LOG=on / non-empty → enable per-instruction trace.
// Anything else (including unset) is off. The check is intentionally
// permissive so users don't have to remember an exact value.
bool log_trace_enabled() {
    const char* v = std::getenv("LOG");
    if (v == nullptr || *v == '\0') return false;
    const std::string s(v);
    if (s == "0" || s == "off" || s == "false" || s == "no") return false;
    return true;
}

} // namespace

int run_full_mode(const SimConfig& cfg, const CliArgs& cli) {
    if (cli.trace) {
        LOG_ERROR("default mode is per-core; pass --trace-dir DIR (with "
                  "p<i>.champsimtrace files) or --trace-list FILE, not --trace");
        return 1;
    }
    if (!cli.trace_dir && !cli.trace_list) {
        LOG_ERROR("default mode requires --trace-dir DIR or --trace-list FILE");
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

    const auto trace_paths = resolve_per_core_traces(cli, cfg.cores);

    const auto settings = to_settings(cfg);
    coherence::CoherenceStats stats;

    // Resolve the run output directory up front so log.rpt (when LOG=1
    // is set) can be opened before any cycles tick. Dir creation
    // failures are non-fatal — we log a warning and skip per-file
    // outputs at the end, matching the existing behavior.
    const std::string proto_label_str = coherence::protocol_label(settings.protocol);
    const fs::path run_dir_pre = build_run_dir_pre(cfg, cli, proto_label_str);
    {
        std::error_code ec;
        fs::create_directories(run_dir_pre, ec);
        if (ec) {
            LOG_WARN("could not create " << run_dir_pre << ": "
                     << ec.message() << "; per-file reports may be skipped");
        }
    }

    // Optional execution-trace log. The TraceLogger holds a reference
    // to the ofstream, so the stream has to outlive the simulation.
    const bool log_on = log_trace_enabled();
    std::ofstream log_stream;
    std::unique_ptr<ooo::TraceLogger> trace_logger;
    if (log_on) {
        log_stream.open(run_dir_pre / "log.rpt");
        if (!log_stream) {
            LOG_WARN("LOG=1 set but could not open " << (run_dir_pre / "log.rpt")
                     << "; trace logging disabled");
        } else {
            trace_logger = std::make_unique<ooo::TraceLogger>(
                log_stream, static_cast<std::size_t>(cfg.cores));
            trace_logger->write_header(trace_label(cli), proto_label_str);
            LOG_INFO("LOG=1: per-instruction trace -> " << (run_dir_pre / "log.rpt"));
        }
    }

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
        if (trace_logger) {
            cs->core->set_trace_logger(trace_logger.get(), i);
        }

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
        cfg, cli, stats, stack_owners, clock, completed, proto_label_str};

    // stdout: short overview only. Detailed numbers live in the per-
    // file reports under run_dir_pre. Helps a sweeper grep summary
    // lines without scrolling through a 200-line dump per run.
    write_overview(std::cout, ctx);

    // Write each report to its own file. Each file is self-contained
    // (header + section); sweep tooling that diffs config.rpt can
    // ignore the noisy stats.rpt.
    auto write_to = [&](const char* fname, auto&& fn) {
        const fs::path p = run_dir_pre / fname;
        if (std::ofstream out(p); out) {
            fn(out);
        } else {
            LOG_WARN("could not open " << p << " for writing");
        }
    };
    write_to("report.rpt",    [&](std::ostream& o){ write_overview(o, ctx); });
    write_to("config.rpt",    [&](std::ostream& o){ write_config_only(o, ctx); });
    write_to("stats.rpt",     [&](std::ostream& o){ write_stats(o, ctx); });
    write_to("coherence.rpt", [&](std::ostream& o){ write_coherence_file(o, ctx); });
    write_to("report.csv",    [&](std::ostream& o){ write_csv(o, ctx); });
    LOG_INFO("wrote reports to " << run_dir_pre);

    return completed ? 0 : 5;
}

} // namespace comparch::full
