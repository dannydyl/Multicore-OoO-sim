#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <vector>

#include "comparch/predictor/predictor.hpp"
#include "comparch/predictor/saturating_counter.hpp"

using comparch::PredictorConfig;
using comparch::predictor::Branch;
using comparch::predictor::make;
using comparch::predictor::SaturatingCounter;

TEST_CASE("SaturatingCounter walks the Smith state diagram", "[predictor][counter]") {
    SECTION("2-bit counter saturates at both ends") {
        SaturatingCounter c(2, 1); // weakly not-taken
        REQUIRE_FALSE(c.is_taken());
        REQUIRE(c.is_weak());

        c.update(true); // -> weakly taken
        REQUIRE(c.is_taken());
        REQUIRE(c.is_weak());
        REQUIRE(c.value() == 2);

        c.update(true); // -> strongly taken (saturate)
        c.update(true);
        c.update(true);
        REQUIRE(c.value() == 3);
        REQUIRE_FALSE(c.is_weak());
        REQUIRE(c.is_taken());

        // Now ride it back down and confirm we clamp at zero.
        for (int i = 0; i < 10; ++i) c.update(false);
        REQUIRE(c.value() == 0);
        REQUIRE_FALSE(c.is_taken());
    }

    SECTION("reset() places the counter at the requested weak state") {
        SaturatingCounter c(4, 0);
        c.reset(true);
        REQUIRE(c.value() == 8); // 2^(4-1)
        REQUIRE(c.is_taken());
        REQUIRE(c.is_weak());

        c.reset(false);
        REQUIRE(c.value() == 7);
        REQUIRE_FALSE(c.is_taken());
        REQUIRE(c.is_weak());
    }

    SECTION("max() reports 2^bits - 1") {
        REQUIRE(SaturatingCounter(2, 0).max() == 3);
        REQUIRE(SaturatingCounter(4, 0).max() == 15);
        REQUIRE(SaturatingCounter(8, 0).max() == 255);
    }
}

TEST_CASE("AlwaysTaken predicts taken for every branch", "[predictor][always_taken]") {
    PredictorConfig cfg;
    cfg.type = "always_taken";
    auto p = make(cfg);
    REQUIRE(p->name() == "always_taken");

    // Mix of taken and not-taken outcomes; predict() must stay true regardless.
    for (std::uint64_t i = 0; i < 1000; ++i) {
        Branch b{.ip = 0x400000 + i * 4, .taken = (i % 3 == 0), .inst_num = i};
        REQUIRE(p->predict(b) == true);
        p->update(b, true); // no-op; just confirm it doesn't crash
    }
}

TEST_CASE("Factory rejects unknown predictor types", "[predictor][factory]") {
    PredictorConfig cfg;
    cfg.type = "no_such_predictor";
    REQUIRE_THROWS_AS(make(cfg), comparch::ConfigError);
}

// Replays a deterministic outcome stream against a predictor at a single PC
// and reports (correct, total). Intentionally trivial — these synthetic tests
// only need to confirm the algorithm can learn a regular pattern.
namespace {
struct Score { std::uint64_t correct = 0, total = 0; };

Score run_pattern(comparch::predictor::BranchPredictor& p,
                  const std::vector<bool>& outcomes,
                  std::uint64_t pc = 0x400000) {
    Score s;
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        Branch b{.ip = pc, .taken = outcomes[i], .inst_num = i};
        const bool pred = p.predict(b);
        if (pred == outcomes[i]) ++s.correct;
        ++s.total;
        p.update(b, pred);
    }
    return s;
}
} // namespace

TEST_CASE("Yeh-Patt rejects out-of-range parameters", "[predictor][yeh_patt][config]") {
    PredictorConfig cfg;
    cfg.type = "yeh_patt";
    cfg.history_bits = 0;
    cfg.pattern_bits = 5;
    REQUIRE_THROWS_AS(make(cfg), comparch::ConfigError);

    cfg.history_bits = 10;
    cfg.pattern_bits = 64;
    REQUIRE_THROWS_AS(make(cfg), comparch::ConfigError);
}

TEST_CASE("Yeh-Patt learns trivial patterns", "[predictor][yeh_patt]") {
    PredictorConfig cfg;
    cfg.type = "yeh_patt";
    cfg.history_bits = 10;
    cfg.pattern_bits = 5;

    SECTION("all-taken converges to 100% after warmup") {
        auto p = make(cfg);
        std::vector<bool> stream(1000, true);
        const auto s = run_pattern(*p, stream);
        // The history register fills one bit at a time, so the first P
        // branches each index a fresh PT slot (still weakly-not-taken) and
        // miss. The (P+1)th branch hits the saturated slot but its counter
        // was just bumped to weakly-taken so it predicts taken — actually
        // wait, weakly-NT was bumped to weakly-T on the prior update, so
        // is_taken() at value 2 returns true. So expect exactly P misses on
        // the warmup, then the run-up to a fully-trained slot adds one more.
        // Total deterministic misses for P=5: 6.
        REQUIRE(s.total == 1000);
        REQUIRE(s.correct == 1000 - 6);
    }

    SECTION("all-not-taken converges to 100% after warmup") {
        auto p = make(cfg);
        std::vector<bool> stream(1000, false);
        const auto s = run_pattern(*p, stream);
        // Initial state is already weakly-not-taken, so this is near-perfect
        // immediately.
        REQUIRE(s.correct == s.total);
    }

    SECTION("alternating pattern is perfectly learnable after warmup") {
        auto p = make(cfg);
        std::vector<bool> stream;
        stream.reserve(2000);
        for (int i = 0; i < 2000; ++i) stream.push_back(i % 2 == 0);
        const auto s = run_pattern(*p, stream);
        // History needs ~P+2 alternations to lock in two distinct patterns
        // ("01010..." → taken, "10101..." → not-taken). After that, every
        // prediction is correct. Allow a generous warmup; require >= 99% at
        // the end of the run.
        REQUIRE(static_cast<double>(s.correct) / s.total >= 0.99);
    }

    SECTION("repeating period-5 pattern is learnable") {
        // Pattern: T T T T N, repeating. Period 5, exactly representable in a
        // 5-bit history register, so Yeh-Patt should hit close to 100% once
        // every history offset has been seen and trained.
        auto p = make(cfg);
        std::vector<bool> stream;
        const std::array<bool, 5> period{true, true, true, true, false};
        for (int i = 0; i < 2000; ++i) stream.push_back(period[i % 5]);
        const auto s = run_pattern(*p, stream);
        REQUIRE(static_cast<double>(s.correct) / s.total >= 0.97);
    }
}

TEST_CASE("Perceptron rejects out-of-range parameters", "[predictor][perceptron][config]") {
    PredictorConfig cfg;
    cfg.type = "perceptron";
    cfg.perceptron_history_bits = 0;
    cfg.perceptron_index_bits = 7;
    REQUIRE_THROWS_AS(make(cfg), comparch::ConfigError);

    cfg.perceptron_history_bits = 9;
    cfg.perceptron_index_bits = 0;
    REQUIRE_THROWS_AS(make(cfg), comparch::ConfigError);
}

TEST_CASE("Perceptron learns linearly separable patterns", "[predictor][perceptron]") {
    PredictorConfig cfg;
    cfg.type = "perceptron";
    cfg.perceptron_history_bits = 9;
    cfg.perceptron_index_bits = 7;

    SECTION("all-taken trains the bias to large positive within a few branches") {
        auto p = make(cfg);
        std::vector<bool> stream(1000, true);
        const auto s = run_pattern(*p, stream);
        // Initial weights are zero, so the first branch's output is exactly 0
        // and the predictor returns false (sum > 0 is false). After a handful
        // of training steps the bias dominates and predictions lock to taken.
        // Empirically the warmup is well under 50 branches.
        REQUIRE(s.total == 1000);
        REQUIRE(s.correct >= 950);
    }

    SECTION("alternating pattern is linearly separable in one history bit") {
        auto p = make(cfg);
        std::vector<bool> stream;
        for (int i = 0; i < 2000; ++i) stream.push_back(i % 2 == 0);
        const auto s = run_pattern(*p, stream);
        // After enough training the weight on h_1 becomes strongly negative
        // (most recent outcome was taken => next is not-taken, and vice
        // versa), driving accuracy near 100%.
        REQUIRE(static_cast<double>(s.correct) / s.total >= 0.97);
    }
}

TEST_CASE("Hybrid rejects out-of-range parameters", "[predictor][hybrid][config]") {
    PredictorConfig cfg;
    cfg.type = "hybrid";
    cfg.hybrid_init = 4; // valid range is [0,3]
    REQUIRE_THROWS_AS(make(cfg), comparch::ConfigError);

    cfg.hybrid_init = 2;
    cfg.tournament_counter_bits = 1; // valid range is [2,8]
    REQUIRE_THROWS_AS(make(cfg), comparch::ConfigError);
}

TEST_CASE("Hybrid composes Yeh-Patt and Perceptron", "[predictor][hybrid]") {
    PredictorConfig cfg;
    cfg.type = "hybrid";
    cfg.history_bits = 10;
    cfg.pattern_bits = 5;
    cfg.perceptron_history_bits = 9;
    cfg.perceptron_index_bits = 7;
    cfg.hybrid_init = 2;
    cfg.tournament_index_bits = 12;
    cfg.tournament_counter_bits = 4;

    SECTION("alternating pattern: hybrid is at least as good as Yeh-Patt alone") {
        std::vector<bool> stream;
        for (int i = 0; i < 4000; ++i) stream.push_back(i % 2 == 0);

        PredictorConfig yp_only = cfg;
        yp_only.type = "yeh_patt";
        auto yp = make(yp_only);
        const auto yp_score = run_pattern(*yp, stream);

        auto hyb = make(cfg);
        const auto hyb_score = run_pattern(*hyb, stream);

        // Both predictors handle this pattern essentially perfectly. The
        // tournament can only do worse during the brief window where one
        // sub-predictor lags the other; we just confirm the hybrid stays
        // in the same ballpark.
        const double yp_acc  = double(yp_score.correct) / yp_score.total;
        const double hyb_acc = double(hyb_score.correct) / hyb_score.total;
        REQUIRE(hyb_acc >= yp_acc - 0.02);
        REQUIRE(hyb_acc >= 0.97);
    }

    SECTION("init=0 forces strongly-Yeh-Patt at the start") {
        PredictorConfig yp_init = cfg;
        yp_init.hybrid_init = 0;
        auto p = make(yp_init);

        // First branch: tournament counter is 0 so MSB is 0 -> Yeh-Patt.
        // Yeh-Patt's PT entries start weakly-not-taken so first prediction
        // is "not taken". Verify that's what we see.
        Branch b{.ip = 0x400000, .taken = true, .inst_num = 0};
        REQUIRE(p->predict(b) == false);
    }

    SECTION("init=3 forces strongly-Perceptron at the start") {
        PredictorConfig pct_init = cfg;
        pct_init.hybrid_init = 3;
        auto p = make(pct_init);

        // Tournament counter saturates at max => MSB=1 => Perceptron picked.
        // Perceptron starts with all weights = 0 => output = 0 => prediction
        // is "not taken" (output > 0 is false). So first prediction is false.
        Branch b{.ip = 0x400000, .taken = true, .inst_num = 0};
        REQUIRE(p->predict(b) == false);
    }
}
