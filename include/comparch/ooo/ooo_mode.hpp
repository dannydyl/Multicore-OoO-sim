#pragma once

#include "comparch/cli.hpp"
#include "comparch/config.hpp"

namespace comparch::ooo {

// Driver entry point for `comparch-sim --mode ooo`. Builds an L1-D / L2 /
// DRAM hierarchy, constructs the configured BranchPredictor, wraps a
// ChampSim trace::Reader, runs an OooCore until the pipeline drains, and
// prints IPC / MPKI / utilization stats.
//
// Returns 0 on success, 2 on config error, 4 on trace error.
int run_ooo_mode(const SimConfig& cfg, const CliArgs& cli);

} // namespace comparch::ooo
