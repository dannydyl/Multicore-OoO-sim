#pragma once

#include "comparch/cli.hpp"
#include "comparch/config.hpp"

namespace comparch::coherence {

// Driver entry point for `comparch-sim --mode coherence`. Builds an N-node
// ring network with one directory node, runs FICI-style per-core trace
// drivers through a coherence protocol agent on each L1, and prints stats
// in project3-compatible format.
//
// Returns 0 on success, 2 on config error, 4 on trace error.
int run_coherence_mode(const SimConfig& cfg, const CliArgs& cli);

} // namespace comparch::coherence
