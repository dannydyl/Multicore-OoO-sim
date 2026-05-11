// Default-mode driver: N OoO cores, each with private L1+L2,
// ring-connected through the coherence subsystem.
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
#include "comparch/program_manifest.hpp"
#include "comparch/sync_coordinator.hpp"
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
constexpr coherence::Timestamp kGlobalCap = 60'000'000;

// Per-core ownership pack. The Network holds non-owning pointers into
// these (CoherenceAdapter as CpuPort, adapter->coh_cache() as the
// per-node coherence::Cache).
struct CoreStack {
    std::unique_ptr<predictor::BranchPredictor>  pred;
    std::unique_ptr<cache::Cache>                l2;   // null in shared_lls
    std::unique_ptr<cache::Cache>                l1;
    std::unique_ptr<coherence::CoherenceAdapter> adapter;
    std::unique_ptr<trace::Reader>               reader;
    std::unique_ptr<ooo::OooCore>                core;
};

// Per-core L2 may be absent in shared_lls mode. The reporting helpers
// below all want a CacheStats reference; this returns a zero-initialized
// static when l2 is null, so the call sites can stay reference-typed
// without per-site null guards.
const cache::CacheStats& l2_stats_or_empty(const CoreStack& cs) {
    static const cache::CacheStats kEmpty;
    return cs.l2 ? cs.l2->stats() : kEmpty;
}

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
    s.cache_mode      = coherence::parse_cache_mode(cfg.coherence.cache_mode);
    s.inclusion       = coherence::parse_inclusion(cfg.coherence.inclusion);
    // The shared-LLS data path implements non-inclusive non-exclusive
    // (NINE): every L1 fill installs the line in LLS, but LLS evictions
    // do NOT back-invalidate L1 holders (see directory.cpp
    // schedule_data_response). Strict inclusive would require a
    // back-invalidate message kind plus per-agent handling for
    // non-S states; that hasn't been implemented yet, so accepting
    // `inclusion: inclusive` silently would mislabel results. Reject
    // it explicitly until the back-invalidate path exists.
    if (s.cache_mode == coherence::CacheMode::SharedLls &&
        s.inclusion  == coherence::Inclusion::Inclusive) {
        throw ConfigError(
            "coherence.inclusion='inclusive' is not yet implemented for "
            "shared_lls mode. The current data path is non-inclusive "
            "non-exclusive (NINE): LLS evictions do not back-invalidate "
            "L1 holders. Use coherence.inclusion='non_inclusive' to make "
            "the label match the actual behavior.");
    }
    // LLS geometry. Always populated so logs/reports can render it even
    // in private_l2 mode (the directory just won't consult an LlsCache).
    const std::size_t bs = static_cast<std::size_t>(cfg.lls.block_size);
    s.lls_blocks      = bs == 0 ? 0
                                : static_cast<std::size_t>(cfg.lls.size_kb) * 1024 / bs;
    s.lls_assoc       = static_cast<std::size_t>(cfg.lls.assoc);
    s.lls_hit_latency = static_cast<std::size_t>(cfg.lls.hit_latency);
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
    if (cli.program)    return cli.program->string();
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
        const auto& l2s = l2_stats_or_empty(*ctx.stacks[i]);
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
        const auto& l2s = l2_stats_or_empty(*ctx.stacks[i]);
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

        // ---- Cycle utilization breakdown -------------------------
        // Two independent partitions of every cycle. Each axis
        // sums to 100% (modulo rounding). The fetch axis tells
        // you why the *frontend* was idle; the retire axis tells
        // you why the *backend* couldn't commit. The headline
        // number is `useful retire cycles` — cycles where at
        // least one instruction committed.
        const auto cyc = s.cycles ? s.cycles : 1;
        auto pct = [&](std::uint64_t v) {
            return fix(100.0 * static_cast<double>(v) /
                       static_cast<double>(cyc), 2) + " %";
        };
        // "Fetch fired" = cycles where fetch added at least one
        // record to dispq. Derived: cyc - (sum of fetch_stall_*).
        const auto fetch_stalls_sum = s.fetch_stall_sync +
                                      s.fetch_stall_dispq_full +
                                      s.fetch_stall_mispred +
                                      s.fetch_stall_eof;
        const auto fetch_fired = (cyc >= fetch_stalls_sum)
                                 ? cyc - fetch_stalls_sum : 0;

        os << "  Cycle utilization (headline)\n";
        kv(os, "    ", "useful retire cycles",
           str(s.useful_retire_cycles) + " (" +
           pct(s.useful_retire_cycles) + ")  <-- pipeline doing useful work");
        kv(os, "    ", "fetch fired cycles",
           str(fetch_fired) + " (" +
           fix(100.0 * static_cast<double>(fetch_fired) /
               static_cast<double>(cyc), 2) + " %)");

        os << "  Backend stall breakdown (sums to 100%)\n";
        kv(os, "    ", "retire fired",
           str(s.useful_retire_cycles) + " (" +
           pct(s.useful_retire_cycles) + ")");
        kv(os, "    ", "retire stall: ROB empty",
           str(s.retire_stall_rob_empty) + " (" +
           pct(s.retire_stall_rob_empty) + ")");
        kv(os, "    ", "retire stall: head not ready",
           str(s.retire_stall_head_busy) + " (" +
           pct(s.retire_stall_head_busy) + ")");

        os << "  Frontend stall breakdown (sums to 100%)\n";
        kv(os, "    ", "fetch fired",
           str(fetch_fired) + " (" +
           fix(100.0 * static_cast<double>(fetch_fired) /
               static_cast<double>(cyc), 2) + " %)");
        kv(os, "    ", "fetch stall: sync (SyncCoordinator)",
           str(s.fetch_stall_sync) + " (" + pct(s.fetch_stall_sync) + ")");
        kv(os, "    ", "fetch stall: dispq full",
           str(s.fetch_stall_dispq_full) + " (" +
           pct(s.fetch_stall_dispq_full) + ")");
        kv(os, "    ", "fetch stall: mispred recovery",
           str(s.fetch_stall_mispred) + " (" +
           pct(s.fetch_stall_mispred) + ")");
        kv(os, "    ", "fetch stall: post-EOF idle",
           str(s.fetch_stall_eof) + " (" + pct(s.fetch_stall_eof) + ")");

        // Per-FU utilization. Each cycle counts at most #FUs of a
        // class as busy; util% is busy-slot-cycles / (cycles * count).
        auto fu_util = [&](std::uint64_t busy_sum, int count) {
            const auto denom = static_cast<double>(cyc) *
                               static_cast<double>(count > 0 ? count : 1);
            return fix(100.0 * static_cast<double>(busy_sum) / denom, 2);
        };
        os << "  Functional unit utilization\n";
        kv(os, "    ", "ALU (count / busy-cycles / per-FU util)",
           str(ctx.cfg.core.alu_fus) + " / " + str(s.alu_busy_sum) +
           " / " + fu_util(s.alu_busy_sum, ctx.cfg.core.alu_fus) + " %");
        kv(os, "    ", "MUL (count / busy-cycles / per-FU util)",
           str(ctx.cfg.core.mul_fus) + " / " + str(s.mul_busy_sum) +
           " / " + fu_util(s.mul_busy_sum, ctx.cfg.core.mul_fus) + " %");
        kv(os, "    ", "LSU (count / busy-cycles / per-FU util)",
           str(ctx.cfg.core.lsu_fus) + " / " + str(s.lsu_busy_sum) +
           " / " + fu_util(s.lsu_busy_sum, ctx.cfg.core.lsu_fus) + " %");

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

    // `cache_accesses` in the shared CoherenceStats is bumped by
    // FiciCpu (used by --mode coherence only). In full mode the L1
    // is driven by the OoO core via the adapter, which doesn't go
    // through FiciCpu, so the counter would read 0. Aggregate per-
    // core L1 accesses instead — same meaning, correct under either
    // mode.
    std::uint64_t l1_accesses_total = 0;
    for (auto& s : ctx.stacks) l1_accesses_total += s->l1->stats().accesses;
    const auto cache_accesses_total =
        cs.cache_accesses != 0 ? cs.cache_accesses : l1_accesses_total;

    write_separator(os, '=', "Coherence + memory");
    kv(os, "", "Protocol",                  ctx.proto_label);
    kv(os, "", "Cache accesses (system)",   str(cache_accesses_total));
    kv(os, "", "Cache misses (system)",     str(cs.cache_misses));
    kv(os, "", "Silent upgrades",           str(cs.silent_upgrades));
    kv(os, "", "Cache-to-cache transfers",  str(cs.c2c_transfers));
    kv(os, "", "Memory reads",              str(cs.memory_reads));
    kv(os, "", "Memory writes",             str(cs.memory_writes));

    // Derived ratios. Useful for comparing protocols at a glance:
    //   c2c_share  = how many misses were satisfied by a peer cache
    //                (intervention) vs. memory.
    if (cs.cache_misses != 0) {
        const double c2c_share = static_cast<double>(cs.c2c_transfers) /
                                 static_cast<double>(cs.cache_misses);
        kv(os, "", "C2C / miss",            fix(c2c_share, 4));
    }
    if (cache_accesses_total != 0) {
        const double system_miss_rate = static_cast<double>(cs.cache_misses) /
                                        static_cast<double>(cache_accesses_total);
        kv(os, "", "System miss rate",      fix(100.0 * system_miss_rate, 3) + " %");
    }

    // System-wide invalidation count (sum across L1+L2 of every core).
    // L2 may be absent in shared_lls mode; fall back to the empty stats.
    std::uint64_t invs_total = 0;
    for (auto& cs2 : ctx.stacks) {
        invs_total += cs2->l1->stats().coherence_invals;
        invs_total += l2_stats_or_empty(*cs2).coherence_invals;
    }
    kv(os, "", "Coherence invalidations",   str(invs_total));

    // Shared-LLS section. Skipped in private_l2 mode because every
    // counter is zero (the directory's LlsCache is disabled) and the
    // section would just add noise to the report. cache_mode is in
    // settings(), accessible via the directory we don't have here -- so
    // gate on whether any counter is non-zero, which works for both
    // modes without plumbing settings into the report context.
    const bool any_lls_activity = (cs.lls_accesses + cs.lls_hits +
                                   cs.lls_misses + cs.lls_evictions +
                                   cs.lls_back_invalidations) > 0;
    if (any_lls_activity) {
        os << '\n';
        write_separator(os, '-', "Shared LLS");
        kv(os, "", "LLS accesses",             str(cs.lls_accesses));
        kv(os, "", "LLS hits",                 str(cs.lls_hits));
        kv(os, "", "LLS misses",               str(cs.lls_misses));
        kv(os, "", "LLS evictions",            str(cs.lls_evictions));
        kv(os, "", "LLS back-invalidations",   str(cs.lls_back_invalidations));
        if (cs.lls_accesses != 0) {
            const double hit_rate = static_cast<double>(cs.lls_hits) /
                                    static_cast<double>(cs.lls_accesses);
            kv(os, "", "LLS hit rate",
               fix(100.0 * hit_rate, 3) + " %");
        }
    }
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

// Body section for utilization.rpt: aggregate over cores plus
// per-core breakdown. Same numbers as the "Cycle utilization"
// block in stats.rpt, but isolated so it's diff-friendly across
// sweep runs and easy to consume for "where did the cycles go"
// pipeline-effectiveness analysis.
void write_utilization(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '-', "System aggregate (across all cores)");

    // Cross-core totals. Each core's `cycles` should equal `ctx.clock`
    // (since tick is driven by the global loop), so dividing by
    // ctx.stacks.size() * ctx.clock gives the average per-core %.
    std::uint64_t agg_cycles            = 0;
    std::uint64_t agg_useful_retire     = 0;
    std::uint64_t agg_fetch_sync        = 0;
    std::uint64_t agg_fetch_dispq_full  = 0;
    std::uint64_t agg_fetch_mispred     = 0;
    std::uint64_t agg_fetch_eof         = 0;
    std::uint64_t agg_retire_rob_empty  = 0;
    std::uint64_t agg_retire_head_busy  = 0;
    std::uint64_t agg_alu_busy_sum      = 0;
    std::uint64_t agg_mul_busy_sum      = 0;
    std::uint64_t agg_lsu_busy_sum      = 0;
    for (auto& cs : ctx.stacks) {
        const auto& s = cs->core->stats();
        agg_cycles           += s.cycles;
        agg_useful_retire    += s.useful_retire_cycles;
        agg_fetch_sync       += s.fetch_stall_sync;
        agg_fetch_dispq_full += s.fetch_stall_dispq_full;
        agg_fetch_mispred    += s.fetch_stall_mispred;
        agg_fetch_eof        += s.fetch_stall_eof;
        agg_retire_rob_empty += s.retire_stall_rob_empty;
        agg_retire_head_busy += s.retire_stall_head_busy;
        agg_alu_busy_sum     += s.alu_busy_sum;
        agg_mul_busy_sum     += s.mul_busy_sum;
        agg_lsu_busy_sum     += s.lsu_busy_sum;
    }
    const auto den = agg_cycles ? agg_cycles : 1;
    auto agg_pct = [&](std::uint64_t v) {
        return fix(100.0 * static_cast<double>(v) /
                   static_cast<double>(den), 2) + " %";
    };
    const auto fetch_fired_agg =
        agg_cycles >= (agg_fetch_sync + agg_fetch_dispq_full +
                       agg_fetch_mispred + agg_fetch_eof)
            ? agg_cycles - (agg_fetch_sync + agg_fetch_dispq_full +
                            agg_fetch_mispred + agg_fetch_eof)
            : 0;

    kv(os, "", "useful retire (any core)",
       str(agg_useful_retire) + " (" + agg_pct(agg_useful_retire) + ")");
    kv(os, "", "fetch fired (any core)",
       str(fetch_fired_agg) + " (" + agg_pct(fetch_fired_agg) + ")");
    os << "  Backend stall (sum across cores)\n";
    kv(os, "    ", "retire fired",
       str(agg_useful_retire) + " (" + agg_pct(agg_useful_retire) + ")");
    kv(os, "    ", "ROB empty",
       str(agg_retire_rob_empty) + " (" + agg_pct(agg_retire_rob_empty) + ")");
    kv(os, "    ", "head not ready",
       str(agg_retire_head_busy) + " (" + agg_pct(agg_retire_head_busy) + ")");
    os << "  Frontend stall (sum across cores)\n";
    kv(os, "    ", "fetch fired",
       str(fetch_fired_agg) + " (" + agg_pct(fetch_fired_agg) + ")");
    kv(os, "    ", "sync (SyncCoordinator)",
       str(agg_fetch_sync) + " (" + agg_pct(agg_fetch_sync) + ")");
    kv(os, "    ", "dispq full",
       str(agg_fetch_dispq_full) + " (" + agg_pct(agg_fetch_dispq_full) + ")");
    kv(os, "    ", "mispred recovery",
       str(agg_fetch_mispred) + " (" + agg_pct(agg_fetch_mispred) + ")");
    kv(os, "    ", "post-EOF idle",
       str(agg_fetch_eof) + " (" + agg_pct(agg_fetch_eof) + ")");

    auto fu_util_agg = [&](std::uint64_t busy_sum, int count) {
        const auto denom = static_cast<double>(agg_cycles) *
                           static_cast<double>(count > 0 ? count : 1);
        return denom > 0
            ? fix(100.0 * static_cast<double>(busy_sum) / denom, 2)
            : std::string("0.00");
    };
    os << "  Functional unit utilization (across cores)\n";
    kv(os, "    ", "ALU per-FU util",
       fu_util_agg(agg_alu_busy_sum, ctx.cfg.core.alu_fus) + " %  (count=" +
       str(ctx.cfg.core.alu_fus) + ", busy-cycles=" + str(agg_alu_busy_sum) + ")");
    kv(os, "    ", "MUL per-FU util",
       fu_util_agg(agg_mul_busy_sum, ctx.cfg.core.mul_fus) + " %  (count=" +
       str(ctx.cfg.core.mul_fus) + ", busy-cycles=" + str(agg_mul_busy_sum) + ")");
    kv(os, "    ", "LSU per-FU util",
       fu_util_agg(agg_lsu_busy_sum, ctx.cfg.core.lsu_fus) + " %  (count=" +
       str(ctx.cfg.core.lsu_fus) + ", busy-cycles=" + str(agg_lsu_busy_sum) + ")");
    os << '\n';

    // Per-core breakdown. Same content as the stats.rpt section
    // but condensed (no surrounding pipeline/cache lines), so this
    // file is short enough to diff against a baseline run.
    write_separator(os, '-', "Per-core breakdown");
    for (std::size_t i = 0; i < ctx.stacks.size(); ++i) {
        const auto& s = ctx.stacks[i]->core->stats();
        const auto cyc = s.cycles ? s.cycles : 1;
        auto pct = [&](std::uint64_t v) {
            return fix(100.0 * static_cast<double>(v) /
                       static_cast<double>(cyc), 2) + " %";
        };
        const auto fetch_stalls_sum = s.fetch_stall_sync +
                                      s.fetch_stall_dispq_full +
                                      s.fetch_stall_mispred +
                                      s.fetch_stall_eof;
        const auto fetch_fired = (cyc >= fetch_stalls_sum)
                                 ? cyc - fetch_stalls_sum : 0;

        os << "[ Core " << i << " ]\n";
        kv(os, "  ", "cycles",                str(s.cycles));
        kv(os, "  ", "useful retire",
           str(s.useful_retire_cycles) + " (" +
           pct(s.useful_retire_cycles) + ")");
        kv(os, "  ", "fetch fired",
           str(fetch_fired) + " (" +
           fix(100.0 * static_cast<double>(fetch_fired) /
               static_cast<double>(cyc), 2) + " %)");
        os << "  Backend (retire) stall — sums to 100%\n";
        kv(os, "    ", "ROB empty",
           str(s.retire_stall_rob_empty) + " (" +
           pct(s.retire_stall_rob_empty) + ")");
        kv(os, "    ", "head not ready",
           str(s.retire_stall_head_busy) + " (" +
           pct(s.retire_stall_head_busy) + ")");
        os << "  Frontend (fetch) stall — sums to 100%\n";
        kv(os, "    ", "sync (SyncCoordinator)",
           str(s.fetch_stall_sync) + " (" + pct(s.fetch_stall_sync) + ")");
        kv(os, "    ", "dispq full",
           str(s.fetch_stall_dispq_full) + " (" +
           pct(s.fetch_stall_dispq_full) + ")");
        kv(os, "    ", "mispred recovery",
           str(s.fetch_stall_mispred) + " (" +
           pct(s.fetch_stall_mispred) + ")");
        kv(os, "    ", "post-EOF idle",
           str(s.fetch_stall_eof) + " (" + pct(s.fetch_stall_eof) + ")");

        auto fu_util = [&](std::uint64_t busy_sum, int count) {
            const auto denom = static_cast<double>(cyc) *
                               static_cast<double>(count > 0 ? count : 1);
            return fix(100.0 * static_cast<double>(busy_sum) / denom, 2);
        };
        os << "  Functional unit per-FU utilization\n";
        kv(os, "    ", "ALU",
           fu_util(s.alu_busy_sum, ctx.cfg.core.alu_fus) + " %  (busy-cycles=" +
           str(s.alu_busy_sum) + ")");
        kv(os, "    ", "MUL",
           fu_util(s.mul_busy_sum, ctx.cfg.core.mul_fus) + " %  (busy-cycles=" +
           str(s.mul_busy_sum) + ")");
        kv(os, "    ", "LSU",
           fu_util(s.lsu_busy_sum, ctx.cfg.core.lsu_fus) + " %  (busy-cycles=" +
           str(s.lsu_busy_sum) + ")");
        if (i + 1 < ctx.stacks.size()) os << '\n';
    }
}

// utilization.rpt — focused pipeline-effectiveness breakdown.
// Same per-core numbers as the corresponding stats.rpt section
// but also includes a cross-core aggregate. Diff-friendly for
// sweep tooling that wants to track "where do the cycles go"
// across configuration changes.
void write_utilization_file(std::ostream& os, const ReportContext& ctx) {
    write_separator(os, '=', "Multicore OoO Simulator -- Pipeline Utilization");
    kv(os, "", "Trace",        trace_label(ctx.cli));
    kv(os, "", "Cores",        str(ctx.cfg.cores));
    kv(os, "", "Total cycles", str(ctx.clock));
    if (ctx.cli.tag) kv(os, "", "Tag", *ctx.cli.tag);
    os << '\n';
    write_utilization(os, ctx);
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
        const auto& l2s = l2_stats_or_empty(*ctx.stacks[i]);
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
    else if (cli.program)    stem = cli.program->stem().string();
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
                  "p<i>.champsimtrace files) or --trace-list FILE or "
                  "--program MANIFEST, not --trace");
        return 1;
    }
    if (!cli.trace_dir && !cli.trace_list && !cli.program) {
        LOG_ERROR("default mode requires --trace-dir DIR, --trace-list FILE, "
                  "or --program MANIFEST");
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

    // --program: multi-thread CasimV2 mode. Parse the manifest up
    // front so we can validate cores==threads before anything else
    // (cheaper to fail here than after we've allocated cache state).
    std::optional<trace::ProgramManifest> manifest;
    std::unique_ptr<sync::SyncCoordinator> sync_coord;
    if (cli.program) {
        try {
            manifest = trace::parse_program_manifest(*cli.program);
        } catch (const trace::ManifestError& e) {
            LOG_ERROR("program manifest: " << e.what());
            return 1;
        }
        if (static_cast<int>(manifest->thread_count) != cfg.cores) {
            LOG_ERROR("--program: manifest threads=" << manifest->thread_count
                      << " but cores=" << cfg.cores
                      << "; v1 requires cores==threads (no scheduler "
                         "multiplexing yet)");
            return 1;
        }
        sync_coord = std::make_unique<sync::SyncCoordinator>(manifest->thread_count);
        LOG_INFO("--program: " << manifest->name << " ("
                 << manifest->thread_count << " threads)");
    }

    // Legacy --trace-dir / --trace-list path; --program supplies its
    // own paths from the manifest below.
    const auto trace_paths = cli.program
        ? manifest->paths
        : resolve_per_core_traces(cli, cfg.cores);

    const auto settings = to_settings(cfg);
    const bool shared_lls = settings.cache_mode == coherence::CacheMode::SharedLls;
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

        // Per-core L2 only exists in private_l2 mode. In shared_lls
        // the LLS lives at the directory and L1 sinks straight into
        // the adapter.
        if (!shared_lls) {
            auto l2_cfg = cache::to_cache_config(cfg.l2);
            cs->l2 = std::make_unique<cache::Cache>(std::move(l2_cfg),
                                                    "L2#" + std::to_string(i));
        }

        // L1 always present. In private_l2 it points at L2 as next_level
        // and L2 sinks to the adapter; in shared_lls L1 sinks directly
        // to the adapter (no intermediate L2). The adapter pointer
        // doesn't exist yet, so wire the L1->adapter sink AFTER the
        // adapter is built (post-construction set_coherence_sink).
        auto l1_cfg = cache::to_cache_config(cfg.l1);
        l1_cfg.next_level = shared_lls ? nullptr : cs->l2.get();
        cs->l1 = std::make_unique<cache::Cache>(std::move(l1_cfg),
                                                "L1#" + std::to_string(i));
        if (cs->l2) cs->l2->set_peer_above(cs->l1.get());

        cs->adapter = std::make_unique<coherence::CoherenceAdapter>(
            i, settings, stats,
            coherence::make_agent_factory(settings.protocol),
            *cs->l1, cs->l2.get());   // l2 nullable; adapter handles null

        // Splice the adapter into whichever cache is the bottom of this
        // core's hierarchy: L2 in private mode, L1 in shared_lls mode.
        if (shared_lls) {
            cs->l1->set_coherence_sink(cs->adapter.get());
        } else {
            cs->l2->set_coherence_sink(cs->adapter.get());
        }

        const auto variant = cli.program ? trace::Variant::CasimV2
                                         : trace::Variant::Standard;
        cs->reader = std::make_unique<trace::Reader>(trace_paths[i], variant);
        if (sync_coord) {
            // 1:1 thread-to-core: tid == core index. Manifest order
            // already enforced this (paths[i] == t<i>).
            cs->reader->set_sync_sink(sync_coord.get(),
                                      static_cast<std::uint32_t>(i));
        }
        cs->core = std::make_unique<ooo::OooCore>(
            to_ooo_config(cfg), *cs->pred, *cs->l1, *cs->reader);
        if (trace_logger) {
            cs->core->set_trace_logger(trace_logger.get(), i);
        }
        // Pre-ThreadScheduler: each core runs one fixed thread,
        // identified by its core index. Once a scheduler exists this
        // becomes a per-context-switch update from the scheduler.
        cs->core->set_active_tid(static_cast<std::uint32_t>(i));

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
    // Grace cycles for net.is_done() to drain after every core has gone
    // idle. On heterogeneous real-trace runs an outstanding coherence
    // transaction can keep is_done() false even after all cores hit EOF
    // and drained their queues; without a cap the loop would spin until
    // kGlobalCap, producing no report.
    constexpr coherence::Timestamp kPostIdleGrace = 10'000;
    coherence::Timestamp post_idle = 0;
    while (true) {
        bool any_core_running = false;
        for (auto& cs : stack_owners) {
            if (cs->core->tick()) any_core_running = true;
        }
        if (!any_core_running && net.is_done()) { completed = true; break; }
        if (!any_core_running) {
            if (++post_idle >= kPostIdleGrace) {
                LOG_ERROR("full mode: all cores idle for " << post_idle
                          << " cycles but net.is_done()=false at "
                          << clock << "; exiting with partial network state");
                break;
            }
        } else {
            post_idle = 0;
        }
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
    write_to("report.rpt",      [&](std::ostream& o){ write_overview(o, ctx); });
    write_to("config.rpt",      [&](std::ostream& o){ write_config_only(o, ctx); });
    write_to("stats.rpt",       [&](std::ostream& o){ write_stats(o, ctx); });
    write_to("coherence.rpt",   [&](std::ostream& o){ write_coherence_file(o, ctx); });
    write_to("utilization.rpt", [&](std::ostream& o){ write_utilization_file(o, ctx); });
    write_to("report.csv",      [&](std::ostream& o){ write_csv(o, ctx); });
    LOG_INFO("wrote reports to " << run_dir_pre);

    return completed ? 0 : 5;
}

} // namespace comparch::full
