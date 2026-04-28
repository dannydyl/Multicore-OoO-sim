// Yeh-Patt two-level adaptive branch predictor.
//
// Reference: Tse-Yu Yeh and Yale N. Patt, "Two-Level Adaptive Training Branch
// Prediction", MICRO-24, 1991.
//
// Two tables:
//
//   History Table (HT)  — 2^H entries. HT[i] holds a P-bit shift register
//                         recording the recent taken/not-taken history for
//                         every PC that hashes to slot i. Indexed by the low
//                         H bits of (PC >> 2). The >> 2 drops two bits because
//                         the original ISA's branches are 4-byte aligned;
//                         shifting reduces aliasing across consecutive PCs.
//
//   Pattern Table (PT)  — 2^P entries, each a 2-bit saturating counter.
//                         Indexed by the P-bit history pulled from HT, so the
//                         predictor effectively asks "the last time this PC
//                         had history pattern X, was it taken?".
//
// predict(): HT_idx = (PC >> 2) mod 2^H ;  history = HT[HT_idx]
//            return PT[history].is_taken()
//
// update():  same lookup, then PT[history] saturating-updates with the actual
//            outcome and the history register shifts the actual outcome in
//            (drop high bit, shift left, OR in the new bit).
//
// Ported from project2_v2.1.0_all/branchsim.cpp:27-168. The only structural
// change is dropping project2's YP_FIFO, which existed to delay PT updates
// across a fake pipeline. --mode predictor predicts and updates synchronously
// on the same record, so the FIFO is dead weight; Phase 4 will reintroduce
// any required delay at the OoO layer.

#include "comparch/predictor/predictor.hpp"
#include "comparch/predictor/saturating_counter.hpp"

#include <sstream>
#include <vector>

namespace comparch::predictor {

namespace {

class YehPatt final : public BranchPredictor {
public:
    YehPatt(int history_bits, int pattern_bits)
        : ht_mask_((1ull << history_bits) - 1ull),
          pt_mask_((1ull << pattern_bits) - 1ull),
          history_table_(static_cast<std::size_t>(1ull << history_bits), 0ull),
          // Pattern entries are 2-bit Smith counters initialized to 01
          // (weakly not-taken), matching project2's PT_entry default.
          pattern_table_(static_cast<std::size_t>(1ull << pattern_bits),
                         SaturatingCounter(2, 1)) {}

    bool predict(const Branch& b) override {
        // Index HT with low H bits of PC>>2, then index PT with the P-bit
        // history pulled from that HT slot.
        const std::uint64_t ht_idx = (b.ip >> 2) & ht_mask_;
        const std::uint64_t pt_idx = history_table_[ht_idx] & pt_mask_;
        return pattern_table_[pt_idx].is_taken();
    }

    void update(const Branch& b, bool /*prediction*/) override {
        const std::uint64_t ht_idx = (b.ip >> 2) & ht_mask_;
        const std::uint64_t pt_idx = history_table_[ht_idx] & pt_mask_;

        // Train the pattern-table counter on the ground-truth outcome.
        pattern_table_[pt_idx].update(b.taken);

        // Shift the outcome bit into the per-PC history register; mask back
        // down to P bits so we only ever index a valid PT slot.
        const std::uint64_t bit = b.taken ? 1ull : 0ull;
        history_table_[ht_idx] = ((history_table_[ht_idx] << 1) | bit) & pt_mask_;
    }

    std::string_view name() const override { return "yeh_patt"; }

private:
    std::uint64_t ht_mask_;
    std::uint64_t pt_mask_;
    std::vector<std::uint64_t>      history_table_;
    std::vector<SaturatingCounter>  pattern_table_;
};

[[nodiscard]] bool in_range(int x, int lo, int hi) { return x >= lo && x <= hi; }

} // namespace

std::unique_ptr<BranchPredictor> make_yeh_patt(const PredictorConfig& cfg) {
    // Tables are sized 2^H and 2^P; cap each well below int width so that
    // (1 << bits) is well-defined and table allocation stays sane.
    if (!in_range(cfg.history_bits, 1, 20) || !in_range(cfg.pattern_bits, 1, 20)) {
        std::ostringstream oss;
        oss << "yeh_patt: history_bits=" << cfg.history_bits
            << " pattern_bits=" << cfg.pattern_bits
            << " (each must be in [1, 20])";
        throw ConfigError(oss.str());
    }
    return std::make_unique<YehPatt>(cfg.history_bits, cfg.pattern_bits);
}

} // namespace comparch::predictor
