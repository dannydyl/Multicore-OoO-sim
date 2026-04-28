// Perceptron branch predictor (Jiménez & Lin).
//
// Reference: Daniel A. Jiménez and Calvin Lin, "Dynamic Branch Prediction with
// Perceptrons", HPCA-7, 2001.
//
// Each branch PC selects one perceptron (a vector of G+1 signed integer
// weights) from a table of 2^N entries. Inputs to the perceptron are the bias
// (constant +1) and the global history register (GHR) bits encoded as +1 for
// taken or -1 for not-taken. The dot product of weights and inputs is the
// "output". Sign(output) is the prediction: positive -> taken, negative or
// zero -> not-taken.
//
// Training is on-line and bounded:
//   - Train iff we mispredicted, OR |output| < theta (output is too weak to
//     trust, even if it happened to be right).
//   - For each weight w_i, w_i += t * x_i where t is +1 (taken) or -1 (not).
//     This nudges weights toward the correlation between each history bit and
//     the actual outcome.
//   - Each weight is clamped to [-theta, theta] so that one history pattern
//     cannot dominate the rest, and so the integer math never overflows.
//
// theta = floor(1.93 * G + 14)  is the closed-form choice from Jiménez 2001
// that minimizes training-set error for typical history lengths.
//
// Ported from project2_v2.1.0_all/branchsim.cpp:174-369. The PCT_fifo that
// project2 used to ferry per-prediction state across a fake pipeline is
// dropped — predict() and update() recompute the dot product from current
// weights/GHR (which haven't changed between the two calls), making the
// predictor stateless between predict/update calls and easier to reason about.

#include "comparch/predictor/predictor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace comparch::predictor {

namespace {

class Perceptron final : public BranchPredictor {
public:
    Perceptron(int history_bits, int index_bits)
        : history_bits_(history_bits),
          theta_(static_cast<int>(std::floor(1.93 * history_bits + 14))),
          pct_mask_((1ull << index_bits) - 1ull),
          ghr_(0),
          // 2^N perceptrons, each holding G+1 zero-initialized weights
          // (1 bias weight + G history-bit weights).
          table_(static_cast<std::size_t>(1ull << index_bits),
                 std::vector<int>(static_cast<std::size_t>(history_bits + 1), 0)) {}

    bool predict(const Branch& b) override {
        // Index into the perceptron table with the low N bits of PC>>2, then
        // dot the weights with the +/-1-encoded history register.
        const std::uint64_t idx = (b.ip >> 2) & pct_mask_;
        return dot_product(idx) > 0;
    }

    void update(const Branch& b, bool prediction) override {
        const std::uint64_t idx = (b.ip >> 2) & pct_mask_;
        const int output = dot_product(idx);
        const int t = b.taken ? +1 : -1;

        // Train on a misprediction OR when |output| is below the confidence
        // threshold. The latter is what keeps weights moving on already-
        // -correct predictions until the perceptron is decisively right.
        if ((prediction != b.taken) || std::abs(output) < theta_) {
            auto& w = table_[idx];
            // Bias weight uses x_0 = 1, so it just adds t.
            w[0] = std::clamp(w[0] + t, -theta_, theta_);
            std::uint64_t h = ghr_;
            for (int i = 0; i < history_bits_; ++i) {
                const int x = (h & 1ull) ? +1 : -1;
                w[i + 1] = std::clamp(w[i + 1] + t * x, -theta_, theta_);
                h >>= 1;
            }
        }

        // Shift outcome into the GHR. Upper bits beyond G are unused — we
        // only ever read the low G bits in dot_product().
        ghr_ = (ghr_ << 1) | (b.taken ? 1ull : 0ull);
    }

    std::string_view name() const override { return "perceptron"; }

private:
    int dot_product(std::uint64_t idx) const {
        const auto& w = table_[idx];
        // x[0] = 1 (bias), so contribution is just w[0].
        int sum = w[0];
        std::uint64_t h = ghr_;
        for (int i = 0; i < history_bits_; ++i) {
            // GHR bit -> +1 if taken, -1 if not-taken.
            const int x = (h & 1ull) ? +1 : -1;
            sum += w[i + 1] * x;
            h >>= 1;
        }
        return sum;
    }

    int history_bits_;
    int theta_;
    std::uint64_t pct_mask_;
    std::uint64_t ghr_;
    std::vector<std::vector<int>> table_;
};

[[nodiscard]] bool in_range(int x, int lo, int hi) { return x >= lo && x <= hi; }

} // namespace

std::unique_ptr<BranchPredictor> make_perceptron(const PredictorConfig& cfg) {
    // G drives both theta and per-perceptron memory; cap at 30 (way past
    // anything reasonable) to keep table allocation sane.
    if (!in_range(cfg.perceptron_history_bits, 1, 30) ||
        !in_range(cfg.perceptron_index_bits, 1, 20)) {
        std::ostringstream oss;
        oss << "perceptron: perceptron_history_bits=" << cfg.perceptron_history_bits
            << " perceptron_index_bits=" << cfg.perceptron_index_bits
            << " (history_bits in [1,30], index_bits in [1,20])";
        throw ConfigError(oss.str());
    }
    return std::make_unique<Perceptron>(cfg.perceptron_history_bits,
                                        cfg.perceptron_index_bits);
}

} // namespace comparch::predictor
