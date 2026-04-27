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

void to_json(nlohmann::json& j, const InterconnectConfig& v) {
    j = nlohmann::json{{"topology", v.topology}, {"link_latency", v.link_latency}};
}
void from_json(const nlohmann::json& j, InterconnectConfig& v) {
    j.at("topology").get_to(v.topology);
    j.at("link_latency").get_to(v.link_latency);
}

void to_json(nlohmann::json& j, const MemoryConfig& v) {
    j = nlohmann::json{{"latency", v.latency}, {"block_size", v.block_size}};
}
void from_json(const nlohmann::json& j, MemoryConfig& v) {
    j.at("latency").get_to(v.latency);
    j.at("block_size").get_to(v.block_size);
}

void to_json(nlohmann::json& j, const PredictorConfig& v) {
    j = nlohmann::json{
        {"type", v.type},
        {"history_bits", v.history_bits},
        {"pattern_bits", v.pattern_bits},
    };
}
void from_json(const nlohmann::json& j, PredictorConfig& v) {
    j.at("type").get_to(v.type);
    j.at("history_bits").get_to(v.history_bits);
    j.at("pattern_bits").get_to(v.pattern_bits);
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
    j.at("fetch_width").get_to(v.fetch_width);
    j.at("rob_entries").get_to(v.rob_entries);
    j.at("schedq_entries_per_fu").get_to(v.schedq_entries_per_fu);
    j.at("alu_fus").get_to(v.alu_fus);
    j.at("mul_fus").get_to(v.mul_fus);
    j.at("lsu_fus").get_to(v.lsu_fus);
    j.at("alu_stages").get_to(v.alu_stages);
    j.at("mul_stages").get_to(v.mul_stages);
    j.at("lsu_stages").get_to(v.lsu_stages);
    j.at("predictor").get_to(v.predictor);
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
    };
}
void from_json(const nlohmann::json& j, CacheLevelConfig& v) {
    j.at("size_kb").get_to(v.size_kb);
    j.at("block_size").get_to(v.block_size);
    j.at("assoc").get_to(v.assoc);
    j.at("replacement").get_to(v.replacement);
    j.at("write_policy").get_to(v.write_policy);
    j.at("prefetcher").get_to(v.prefetcher);
    j.at("hit_latency").get_to(v.hit_latency);
}

void to_json(nlohmann::json& j, const CoherenceConfig& v) {
    j = nlohmann::json{{"protocol", v.protocol}};
}
void from_json(const nlohmann::json& j, CoherenceConfig& v) {
    j.at("protocol").get_to(v.protocol);
}

void to_json(nlohmann::json& j, const SimConfig& v) {
    j = nlohmann::json{
        {"cores", v.cores},
        {"interconnect", v.interconnect},
        {"memory", v.memory},
        {"core", v.core},
        {"l1", v.l1},
        {"l2", v.l2},
        {"coherence", v.coherence},
    };
}
void from_json(const nlohmann::json& j, SimConfig& v) {
    j.at("cores").get_to(v.cores);
    j.at("interconnect").get_to(v.interconnect);
    j.at("memory").get_to(v.memory);
    j.at("core").get_to(v.core);
    j.at("l1").get_to(v.l1);
    j.at("l2").get_to(v.l2);
    j.at("coherence").get_to(v.coherence);
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
