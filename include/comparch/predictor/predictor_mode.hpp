#pragma once

#include "comparch/cli.hpp"
#include "comparch/config.hpp"

namespace comparch::predictor {

// Run --mode predictor: walks the trace, drives the configured branch
// predictor on every is_branch record, prints accuracy / misprediction stats.
int run_predictor_mode(const SimConfig& cfg, const CliArgs& cli);

} // namespace comparch::predictor
