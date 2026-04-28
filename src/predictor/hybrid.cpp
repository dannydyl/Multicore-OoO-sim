// Hybrid (tournament) branch predictor.
//
// Reference: Scott McFarling, "Combining Branch Predictors", DEC WRL TN-36,
// 1993.
//
// The hybrid runs two independent direction predictors in parallel — Yeh-Patt
// and Perceptron — and uses a per-PC tournament selector to decide whose
// prediction to actually use on each branch.
//
//   selector table  — 2^TI entries, each a TC-bit saturating counter.
//                     Indexed by the low TI bits of (PC >> 2). The MSB of the
//                     counter selects the predictor: MSB == 1 chooses
//                     Perceptron, MSB == 0 chooses Yeh-Patt.
//
//   selector update — only when the two component predictors disagreed:
//                       - if Yeh-Patt was right, decrement (toward YP)
//                       - if Perceptron was right, increment (toward PCT)
//                     Agreement => no movement; we have no signal to act on.
//
// hybrid_init seeds every selector counter at one of four canonical states,
// matching project2's T parameter:
//
//   T=0 -> 0           (strongly Yeh-Patt)
//   T=1 -> half - 1    (weakly Yeh-Patt)
//   T=2 -> half        (weakly Perceptron)
//   T=3 -> max         (strongly Perceptron)
//
// where half = 2^(TC-1), max = 2^TC - 1. With the default TC=4 these are
// 0, 7, 8, 15 — the project2 numbers verbatim.
//
// Ported from project2_v2.1.0_all/branchsim.cpp:372-515. The TNMT_fifo is
// dropped; we just remember the two component predictions in a small struct
// between predict() and update() because the call sequence is one transaction
// per branch.

#include "comparch/predictor/predictor.hpp"
#include "comparch/predictor/saturating_counter.hpp"

#include <sstream>
#include <vector>

namespace comparch::predictor {

// Forward declarations of the sub-predictor factory entry points so we can
// compose them without depending on internal class definitions.
std::unique_ptr<BranchPredictor> make_yeh_patt(const PredictorConfig& cfg);
std::unique_ptr<BranchPredictor> make_perceptron(const PredictorConfig& cfg);

namespace {

unsigned init_value_for(int hybrid_init, int counter_bits) {
    const unsigned half = 1u << (counter_bits - 1);
    const unsigned max  = (1u << counter_bits) - 1u;
    switch (hybrid_init) {
        case 0: return 0u;
        case 1: return half - 1u;
        case 2: return half;
        case 3: return max;
        default: return half; // unreachable; guarded by the factory
    }
}

class Hybrid final : public BranchPredictor {
public:
    Hybrid(const PredictorConfig& cfg)
        : yp_(make_yeh_patt(cfg)),
          pct_(make_perceptron(cfg)),
          tnmt_mask_((1ull << cfg.tournament_index_bits) - 1ull),
          tnmt_table_(static_cast<std::size_t>(1ull << cfg.tournament_index_bits),
                      SaturatingCounter(cfg.tournament_counter_bits,
                                        init_value_for(cfg.hybrid_init,
                                                       cfg.tournament_counter_bits))) {}

    bool predict(const Branch& b) override {
        const std::uint64_t idx = (b.ip >> 2) & tnmt_mask_;
        // The MSB of the selector counter picks which sub-predictor to trust
        // for this branch. Low bits encode confidence in that choice.
        const bool use_perceptron = tnmt_table_[idx].is_taken();

        // Always run BOTH sub-predictors so they keep training on every
        // branch — otherwise the unselected one would never see new history
        // and stop being a useful comparison.
        last_yp_  = yp_->predict(b);
        last_pct_ = pct_->predict(b);
        return use_perceptron ? last_pct_ : last_yp_;
    }

    void update(const Branch& b, bool /*prediction*/) override {
        const std::uint64_t idx = (b.ip >> 2) & tnmt_mask_;

        // Selector training: only when the two predictors disagreed do we
        // have meaningful signal about which one was right on this branch.
        if (last_yp_ != last_pct_) {
            if (last_yp_ == b.taken) {
                // Yeh-Patt was right; nudge toward YP (decrement).
                tnmt_table_[idx].update(false);
            } else {
                // Perceptron was right; nudge toward PCT (increment).
                tnmt_table_[idx].update(true);
            }
        }

        // Train both sub-predictors on the actual outcome regardless of which
        // was selected — they each maintain independent state.
        yp_->update(b, last_yp_);
        pct_->update(b, last_pct_);
    }

    std::string_view name() const override { return "hybrid"; }

private:
    std::unique_ptr<BranchPredictor> yp_;
    std::unique_ptr<BranchPredictor> pct_;
    std::uint64_t tnmt_mask_;
    std::vector<SaturatingCounter> tnmt_table_;

    // Last predictions emitted by each sub-predictor for the current branch.
    // Cached from predict() so update() can decide whether to move the
    // tournament counter and how to train each sub-predictor.
    bool last_yp_  = false;
    bool last_pct_ = false;
};

[[nodiscard]] bool in_range(int x, int lo, int hi) { return x >= lo && x <= hi; }

} // namespace

std::unique_ptr<BranchPredictor> make_hybrid(const PredictorConfig& cfg) {
    if (!in_range(cfg.tournament_index_bits, 1, 24) ||
        !in_range(cfg.tournament_counter_bits, 2, 8) ||
        !in_range(cfg.hybrid_init, 0, 3)) {
        std::ostringstream oss;
        oss << "hybrid: tournament_index_bits=" << cfg.tournament_index_bits
            << " tournament_counter_bits=" << cfg.tournament_counter_bits
            << " hybrid_init=" << cfg.hybrid_init
            << " (index in [1,24], counter in [2,8], init in [0,3])";
        throw ConfigError(oss.str());
    }
    return std::make_unique<Hybrid>(cfg);
}

} // namespace comparch::predictor
