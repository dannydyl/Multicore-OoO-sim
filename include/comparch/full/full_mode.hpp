#pragma once

#include "comparch/cli.hpp"
#include "comparch/config.hpp"

namespace comparch::full {

// Default-mode driver: builds N OoO cores, each with private L1+L2,
// connected through the Phase 5A coherence ring, and runs them until
// every core's per-core ChampSim trace is exhausted.
//
// Returns 0 on success, 2 on config error, 4 on trace error, 5 on
// global cycle cap (deadlock).
int run_full_mode(const SimConfig& cfg, const CliArgs& cli);

} // namespace comparch::full
