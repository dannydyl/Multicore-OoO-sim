// predictor_mode
// ==============
// Driver for `comparch-sim --mode predictor`. Walks a ChampSim trace, hands
// each is_branch=true record to the configured BranchPredictor, and prints
// accuracy / mispredictions / MPKI at the end.
//
// The walk loop is deliberately a single-record-at-a-time pass: predict,
// compare against ground truth, train. No pipeline, no lookahead, no FIFO —
// any timing nuance belongs in Phase 4 (the OoO core), not here.

#include "comparch/predictor/predictor_mode.hpp"

#include <cstdint>
#include <iomanip>
#include <iostream>

#include "comparch/log.hpp"
#include "comparch/predictor/predictor.hpp"
#include "comparch/trace.hpp"

namespace comparch::predictor {

namespace {

struct Stats {
    std::uint64_t records       = 0; // total trace records (branches + non-branches)
    std::uint64_t branches      = 0; // is_branch records
    std::uint64_t correct       = 0; // predicted == taken
    std::uint64_t mispredicted  = 0; // predicted != taken
};

void print_stats(std::ostream& os, std::string_view name, const Stats& s) {
    // Tiny inline percentage helper; mirrors the one in cache_mode.cpp.
    const auto pct = [](std::uint64_t num, std::uint64_t den) {
        return den == 0 ? 0.0
                        : 100.0 * static_cast<double>(num) / static_cast<double>(den);
    };

    os << "==== branch predictor stats ====\n";
    os << "  predictor          " << name << '\n';
    os << "  records            " << s.records      << '\n';
    os << "  branches           " << s.branches     << "  ("
       << std::fixed << std::setprecision(2)
       << pct(s.branches, s.records) << " %)\n";
    os << "  correct            " << s.correct      << "  ("
       << std::fixed << std::setprecision(2)
       << pct(s.correct, s.branches) << " %)\n";
    os << "  mispredicted       " << s.mispredicted << "  ("
       << std::fixed << std::setprecision(2)
       << pct(s.mispredicted, s.branches) << " %)\n";
    // MPKI = mispredictions per kilo-instruction. Standard reporting unit
    // for branch-prediction work because it's comparable across workloads
    // and predictor depths.
    os << "  MPKI               "
       << std::fixed << std::setprecision(3)
       << (s.records == 0
               ? 0.0
               : 1000.0 * static_cast<double>(s.mispredicted)
                       / static_cast<double>(s.records))
       << '\n';
}

} // namespace

int run_predictor_mode(const SimConfig& cfg, const CliArgs& cli) {
    if (!cli.trace) {
        LOG_ERROR("--mode predictor requires --trace");
        return 2;
    }

    auto pred = make(cfg.predictor);
    LOG_INFO("predictor mode: " << pred->name()
             << " (history=" << cfg.predictor.history_bits
             << " pattern=" << cfg.predictor.pattern_bits << ")");

    // Walk the trace. For each is_branch record:
    //   1. Build a Branch from (ip, branch_taken, instruction_count).
    //   2. predict() — the answer must not depend on br.taken.
    //   3. score against the ground truth.
    //   4. update() — train on what actually happened.
    trace::Reader reader(*cli.trace, trace::Variant::Standard);
    trace::Record rec{};
    Stats s;
    while (reader.next(rec)) {
        ++s.records;
        if (!rec.is_branch) continue;

        const Branch b{
            .ip       = rec.ip,
            .taken    = rec.branch_taken,
            .inst_num = s.records,
        };
        const bool prediction = pred->predict(b);
        if (prediction == rec.branch_taken) {
            ++s.correct;
        } else {
            ++s.mispredicted;
        }
        ++s.branches;
        pred->update(b, prediction);
    }

    LOG_INFO("walked " << s.records << " records (" << s.branches << " branches)");

    print_stats(std::cout, pred->name(), s);
    return 0;
}

} // namespace comparch::predictor
