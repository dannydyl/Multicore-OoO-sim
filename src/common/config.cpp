#include "comparch/config.hpp"

#include <fstream>
#include <sstream>

namespace comparch {

std::string_view to_string(Mode m) {
    switch (m) {
        case Mode::Full:      return "full";
        case Mode::Cache:     return "cache";
        case Mode::Predictor: return "predictor";
        case Mode::Ooo:       return "ooo";
        case Mode::Coherence: return "coherence";
    }
    return "?";
}

std::optional<Mode> parse_mode(std::string_view s) {
    if (s == "full")      return Mode::Full;
    if (s == "cache")     return Mode::Cache;
    if (s == "predictor") return Mode::Predictor;
    if (s == "ooo")       return Mode::Ooo;
    if (s == "coherence") return Mode::Coherence;
    return std::nullopt;
}

// All from_json overloads use j.value("key", v.field) which falls back to the
// struct's default (held in `v` because nlohmann constructs a default-initialized
// instance before calling from_json). This means a partial JSON like
// `{"predictor": {"type": "perceptron"}}` fills in every other field from
// the C++ defaults instead of throwing on the first missing key. Type
// mismatches still throw, so a misspelled key value (e.g. cores: "four") is
// still caught.

void to_json(nlohmann::json& j, const InterconnectConfig& v) {
    j = nlohmann::json{{"topology", v.topology}, {"link_latency", v.link_latency}};
}
void from_json(const nlohmann::json& j, InterconnectConfig& v) {
    v.topology     = j.value("topology",     v.topology);
    v.link_latency = j.value("link_latency", v.link_latency);
}

void to_json(nlohmann::json& j, const MemoryConfig& v) {
    j = nlohmann::json{{"latency", v.latency}, {"block_size", v.block_size}};
}
void from_json(const nlohmann::json& j, MemoryConfig& v) {
    v.latency    = j.value("latency",    v.latency);
    v.block_size = j.value("block_size", v.block_size);
}

void to_json(nlohmann::json& j, const PredictorConfig& v) {
    j = nlohmann::json{
        {"type", v.type},
        {"history_bits", v.history_bits},
        {"pattern_bits", v.pattern_bits},
        {"perceptron_history_bits", v.perceptron_history_bits},
        {"perceptron_index_bits",   v.perceptron_index_bits},
        {"hybrid_init",             v.hybrid_init},
        {"tournament_index_bits",   v.tournament_index_bits},
        {"tournament_counter_bits", v.tournament_counter_bits},
    };
}
void from_json(const nlohmann::json& j, PredictorConfig& v) {
    v.type                    = j.value("type",                    v.type);
    v.history_bits            = j.value("history_bits",            v.history_bits);
    v.pattern_bits            = j.value("pattern_bits",            v.pattern_bits);
    v.perceptron_history_bits = j.value("perceptron_history_bits", v.perceptron_history_bits);
    v.perceptron_index_bits   = j.value("perceptron_index_bits",   v.perceptron_index_bits);
    v.hybrid_init             = j.value("hybrid_init",             v.hybrid_init);
    v.tournament_index_bits   = j.value("tournament_index_bits",   v.tournament_index_bits);
    v.tournament_counter_bits = j.value("tournament_counter_bits", v.tournament_counter_bits);
}

void to_json(nlohmann::json& j, const CoreConfig& v) {
    j = nlohmann::json{
        {"fetch_width", v.fetch_width},
        {"rob_entries", v.rob_entries},
        {"schedq_entries_per_fu", v.schedq_entries_per_fu},
        {"alu_fus", v.alu_fus},
        {"mul_fus", v.mul_fus},
        {"lsu_fus", v.lsu_fus},
        {"alu_stages", v.alu_stages},
        {"mul_stages", v.mul_stages},
        {"lsu_stages", v.lsu_stages},
        {"predictor", v.predictor},
    };
}
void from_json(const nlohmann::json& j, CoreConfig& v) {
    v.fetch_width           = j.value("fetch_width",           v.fetch_width);
    v.rob_entries           = j.value("rob_entries",           v.rob_entries);
    v.schedq_entries_per_fu = j.value("schedq_entries_per_fu", v.schedq_entries_per_fu);
    v.alu_fus               = j.value("alu_fus",               v.alu_fus);
    v.mul_fus               = j.value("mul_fus",               v.mul_fus);
    v.lsu_fus               = j.value("lsu_fus",               v.lsu_fus);
    v.alu_stages            = j.value("alu_stages",            v.alu_stages);
    v.mul_stages            = j.value("mul_stages",            v.mul_stages);
    v.lsu_stages            = j.value("lsu_stages",            v.lsu_stages);
    v.predictor             = j.value("predictor",             v.predictor);
}

void to_json(nlohmann::json& j, const CacheLevelConfig& v) {
    j = nlohmann::json{
        {"size_kb", v.size_kb},
        {"block_size", v.block_size},
        {"assoc", v.assoc},
        {"replacement", v.replacement},
        {"write_policy", v.write_policy},
        {"prefetcher", v.prefetcher},
        {"hit_latency", v.hit_latency},
        {"n_markov_rows", v.n_markov_rows},
    };
}
void from_json(const nlohmann::json& j, CacheLevelConfig& v) {
    v.size_kb       = j.value("size_kb",       v.size_kb);
    v.block_size    = j.value("block_size",    v.block_size);
    v.assoc         = j.value("assoc",         v.assoc);
    v.replacement   = j.value("replacement",   v.replacement);
    v.write_policy  = j.value("write_policy",  v.write_policy);
    v.prefetcher    = j.value("prefetcher",    v.prefetcher);
    v.hit_latency   = j.value("hit_latency",   v.hit_latency);
    v.n_markov_rows = j.value("n_markov_rows", v.n_markov_rows);
}

void to_json(nlohmann::json& j, const CoherenceConfig& v) {
    j = nlohmann::json{{"protocol", v.protocol}};
}
void from_json(const nlohmann::json& j, CoherenceConfig& v) {
    v.protocol = j.value("protocol", v.protocol);
}

void to_json(nlohmann::json& j, const SimConfig& v) {
    j = nlohmann::json{
        {"cores", v.cores},
        {"interconnect", v.interconnect},
        {"memory", v.memory},
        {"core", v.core},
        {"l1", v.l1},
        {"l2", v.l2},
        {"predictor", v.predictor},
        {"coherence", v.coherence},
    };
}
void from_json(const nlohmann::json& j, SimConfig& v) {
    v.cores        = j.value("cores",        v.cores);
    v.interconnect = j.value("interconnect", v.interconnect);
    v.memory       = j.value("memory",       v.memory);
    v.core         = j.value("core",         v.core);
    v.l1           = j.value("l1",           v.l1);
    v.l2           = j.value("l2",           v.l2);
    v.predictor    = j.value("predictor",    v.predictor);
    v.coherence    = j.value("coherence",    v.coherence);
}

SimConfig load_config(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) {
        std::ostringstream oss;
        oss << "cannot open config file: " << path;
        throw ConfigError(oss.str());
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const nlohmann::json::parse_error& e) {
        std::ostringstream oss;
        oss << "JSON parse error in " << path << ": " << e.what();
        throw ConfigError(oss.str());
    }
    SimConfig cfg;
    try {
        cfg = j.get<SimConfig>();
    } catch (const nlohmann::json::exception& e) {
        std::ostringstream oss;
        oss << "config error in " << path << ": " << e.what();
        throw ConfigError(oss.str());
    }
    return cfg;
}

std::string dump_config(const SimConfig& cfg, int indent) {
    nlohmann::json j = cfg;
    return j.dump(indent);
}

} // namespace comparch
