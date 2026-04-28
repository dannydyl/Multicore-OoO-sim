// Always-Taken: the trivial baseline predictor. predict() returns true for
// every branch; update() is a no-op since there is no state to train. Useful
// as a sanity floor — any non-trivial predictor should beat it on real
// workloads, which is why every paper in the area reports it as a baseline.
//
// Ported from project2_v2.1.0_all/branchsim.cpp:18-24, where the equivalent
// function-pointer table just returned true unconditionally.

#include "comparch/predictor/predictor.hpp"

namespace comparch::predictor {

namespace {

class AlwaysTaken final : public BranchPredictor {
public:
    bool predict(const Branch&) override { return true; }
    void update(const Branch&, bool) override { /* nothing to learn */ }
    std::string_view name() const override { return "always_taken"; }
};

} // namespace

std::unique_ptr<BranchPredictor> make_always_taken() {
    return std::make_unique<AlwaysTaken>();
}

} // namespace comparch::predictor
