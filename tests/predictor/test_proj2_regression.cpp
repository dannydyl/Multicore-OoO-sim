// Cross-validates the ported predictors against project2's reference numbers.
//
// branchsim.champsimtrace was converted (via tools/proj2_to_champsim) from a
// deterministic project2 text trace; branchsim.expected.txt holds the
// (correct, mispredicted) counts that project2's `proj2sim -x -b N` emitted
// when run on that exact text trace with default parameters.
//
// If this test ever fails:
//   1. Confirm the trace file wasn't accidentally regenerated against a
//      different text source. See fixtures/proj2/README.md.
//   2. Otherwise the algorithm has drifted from project2 — do not "fix" the
//      test; investigate the predictor code.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "comparch/predictor/predictor.hpp"
#include "comparch/trace.hpp"

using comparch::PredictorConfig;
using comparch::predictor::Branch;
using comparch::predictor::make;

namespace {

const std::filesystem::path kFixtureDir = PROJ2_FIXTURE_DIR;

struct Expected {
    std::uint64_t correct;
    std::uint64_t mispredicted;
};

std::map<std::string, Expected> load_expected(const std::filesystem::path& path) {
    std::ifstream in(path);
    REQUIRE(in.good());
    std::map<std::string, Expected> out;
    std::string line;
    while (std::getline(in, line)) {
        // Skip comment / blank lines (the file leads with a # block).
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        std::string name;
        Expected e{};
        is >> name >> e.correct >> e.mispredicted;
        REQUIRE(is);
        out.emplace(name, e);
    }
    return out;
}

// Walk the fixture trace once, driving the named predictor with the same
// per-branch loop the live --mode predictor uses (predict, score, update).
Expected run(const std::string& predictor_name) {
    PredictorConfig cfg;
    cfg.type = predictor_name;
    // Defaults match project2's defaults: H=10 P=5 G=9 N=7 T=2.
    auto p = make(cfg);

    comparch::trace::Reader reader(kFixtureDir / "branchsim.champsimtrace",
                                   comparch::trace::Variant::Standard);
    comparch::trace::Record rec{};
    Expected got{};
    while (reader.next(rec)) {
        if (!rec.is_branch) continue;
        const Branch b{.ip = rec.ip, .taken = rec.branch_taken, .inst_num = 0};
        const bool prediction = p->predict(b);
        if (prediction == rec.branch_taken) ++got.correct;
        else                                ++got.mispredicted;
        p->update(b, prediction);
    }
    return got;
}

} // namespace

TEST_CASE("proj2 regression: predictor counts match project2 reference",
          "[predictor][proj2-regression]") {
    const auto expected = load_expected(kFixtureDir / "branchsim.expected.txt");
    REQUIRE(expected.size() == 4);

    for (const auto& predictor_name :
         {"always_taken", "yeh_patt", "perceptron", "hybrid"}) {
        SECTION(predictor_name) {
            const auto e = expected.at(predictor_name);
            const auto got = run(predictor_name);

            // Algorithms are deterministic — counts must match exactly.
            REQUIRE(got.correct      == e.correct);
            REQUIRE(got.mispredicted == e.mispredicted);
        }
    }
}
