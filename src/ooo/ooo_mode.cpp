// ooo_mode
// ========
// Driver for `comparch-sim --mode ooo`. Builds an L1-D / L2 / DRAM
// hierarchy (the I-cache is idealized in this phase, see core.hpp),
// constructs the configured BranchPredictor, and runs an OooCore
// against a ChampSim trace to completion.

#include "comparch/ooo/ooo_mode.hpp"

#include <iomanip>
#include <iostream>

#include "comparch/cache/cache.hpp"
#include "comparch/cache/cache_mode.hpp"   // to_cache_config / to_memory_config
#include "comparch/cache/main_memory.hpp"
#include "comparch/log.hpp"
#include "comparch/ooo/core.hpp"
#include "comparch/predictor/predictor.hpp"
#include "comparch/trace.hpp"

namespace comparch::ooo {

namespace {

void print_stats(std::ostream& os, const OooStats& s) {
    const auto pct = [](std::uint64_t num, std::uint64_t den) {
        return den == 0
                   ? 0.0
                   : 100.0 * static_cast<double>(num) / static_cast<double>(den);
    };

    os << "==== ooo stats ====\n";
    os << "  cycles                 " << s.cycles                  << '\n';
    os << "  instructions_fetched   " << s.instructions_fetched    << '\n';
    os << "  instructions_retired   " << s.instructions_retired    << '\n';
    os << "  ipc                    "
       << std::fixed << std::setprecision(3) << s.ipc() << '\n';

    os << "  branches_retired       " << s.num_branch_instructions << '\n';
    os << "  branch_mispredicts     " << s.branch_mispredictions   << "  ("
       << std::fixed << std::setprecision(2)
       << pct(s.branch_mispredictions, s.num_branch_instructions) << " %)\n";
    os << "  MPKI                   "
       << std::fixed << std::setprecision(3)
       << (s.instructions_retired == 0
               ? 0.0
               : 1000.0 * static_cast<double>(s.branch_mispredictions)
                       / static_cast<double>(s.instructions_retired))
       << '\n';

    os << "  no_fire_cycles         " << s.no_fire_cycles          << '\n';
    os << "  rob_no_dispatch_cycles " << s.rob_no_dispatch_cycles  << '\n';

    os << "  dispq max / avg        " << s.dispq_max  << "  / "
       << std::fixed << std::setprecision(2) << s.dispq_avg()  << '\n';
    os << "  schedq max / avg       " << s.schedq_max << "  / "
       << std::fixed << std::setprecision(2) << s.schedq_avg() << '\n';
    os << "  rob max / avg          " << s.rob_max    << "  / "
       << std::fixed << std::setprecision(2) << s.rob_avg()    << '\n';
}

} // namespace

int run_ooo_mode(const SimConfig& cfg, const CliArgs& cli) {
    if (!cli.trace) {
        LOG_ERROR("--mode ooo requires --trace");
        return 2;
    }

    // ---- Build the cache hierarchy bottom-up (DRAM <- L2 <- L1-D).
    cache::MainMemory mem(cache::to_memory_config(cfg.memory));

    auto l2_cc = cache::to_cache_config(cfg.l2);
    l2_cc.main_memory = &mem;
    cache::Cache l2(std::move(l2_cc), "L2");

    auto l1d_cc = cache::to_cache_config(cfg.l1);
    l1d_cc.next_level = &l2;
    cache::Cache l1d(std::move(l1d_cc), "L1-D");

    l2.set_peer_above(&l1d);

    // ---- Predictor. We use the per-core predictor block; the top-level
    // `predictor` block is reserved for --mode predictor.
    auto pred = predictor::make(cfg.core.predictor);

    // ---- OooCore.
    OooConfig occ;
    occ.fetch_width           = static_cast<std::size_t>(cfg.core.fetch_width);
    occ.rob_entries           = static_cast<std::size_t>(cfg.core.rob_entries);
    occ.schedq_entries_per_fu = static_cast<std::size_t>(cfg.core.schedq_entries_per_fu);
    occ.alu_fus               = static_cast<std::size_t>(cfg.core.alu_fus);
    occ.mul_fus               = static_cast<std::size_t>(cfg.core.mul_fus);
    occ.lsu_fus               = static_cast<std::size_t>(cfg.core.lsu_fus);

    LOG_INFO("ooo mode: predictor=" << pred->name()
             << " fetch=" << occ.fetch_width
             << " rob="   << occ.rob_entries
             << " alu="   << occ.alu_fus
             << " mul="   << occ.mul_fus
             << " lsu="   << occ.lsu_fus);

    trace::Reader reader(*cli.trace, trace::Variant::Standard);
    OooCore core(occ, *pred, l1d, reader);
    core.run();

    print_stats(std::cout, core.stats());
    return 0;
}

} // namespace comparch::ooo
