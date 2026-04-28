#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "comparch/config.hpp"

namespace comparch::predictor {

// A single branch event handed to the predictor. Mirrors project2's branch_t
// but uses the field names the rest of the sim already speaks (ip, taken).
//
// `ip`        — PC of the branch instruction.
// `taken`     — ground-truth outcome. Only meaningful in update(); ignored by
//               predict() (otherwise we'd be cheating).
// `inst_num`  — dynamic instruction count at the point of the branch. Carried
//               for stats and debugging; predictors are free to ignore it.
struct Branch {
    std::uint64_t ip       = 0;
    bool          taken    = false;
    std::uint64_t inst_num = 0;
};

// Polymorphic interface every direction predictor implements. The lifecycle is:
//   1. Construct from a PredictorConfig (via make()).
//   2. For each branch:  bool p = predictor->predict(b);
//                        predictor->update(b, p);
// Predict and update are intentionally separate so a future OoO pipeline can
// delay update() until the branch resolves.
class BranchPredictor {
public:
    virtual ~BranchPredictor() = default;

    // Direction prediction. Returns true if predicted taken.
    virtual bool predict(const Branch& b) = 0;

    // Train on the actual outcome of the branch. `prediction` is what we
    // returned from predict() — some predictors (notably hybrid/tournament)
    // need it to reward or penalize the selector.
    virtual void update(const Branch& b, bool prediction) = 0;

    // Short identifier ("always_taken", "yeh_patt", "perceptron", "hybrid")
    // used in stats output and test diagnostics.
    virtual std::string_view name() const = 0;
};

// Construct the predictor named by cfg.type. Throws comparch::ConfigError on
// an unknown type.
std::unique_ptr<BranchPredictor> make(const PredictorConfig& cfg);

} // namespace comparch::predictor
