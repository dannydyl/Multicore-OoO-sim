#include "comparch/predictor/predictor.hpp"

#include <sstream>

namespace comparch::predictor {

// Per-predictor constructors are defined in their own translation units; the
// factory just dispatches on cfg.type. Adding a new predictor means: implement
// it in a new .cpp, declare the make_*() helper here, and add a branch below.
std::unique_ptr<BranchPredictor> make_always_taken();
std::unique_ptr<BranchPredictor> make_yeh_patt(const PredictorConfig& cfg);
std::unique_ptr<BranchPredictor> make_perceptron(const PredictorConfig& cfg);
std::unique_ptr<BranchPredictor> make_hybrid(const PredictorConfig& cfg);

std::unique_ptr<BranchPredictor> make(const PredictorConfig& cfg) {
    if (cfg.type == "always_taken") return make_always_taken();
    if (cfg.type == "yeh_patt")     return make_yeh_patt(cfg);
    if (cfg.type == "perceptron")   return make_perceptron(cfg);
    if (cfg.type == "hybrid")       return make_hybrid(cfg);

    std::ostringstream oss;
    oss << "unknown predictor type: '" << cfg.type << "'";
    throw ConfigError(oss.str());
}

} // namespace comparch::predictor
