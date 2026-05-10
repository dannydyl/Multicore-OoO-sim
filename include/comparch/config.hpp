#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace comparch {

enum class Mode { Full, Cache, Predictor, Ooo, Coherence };

std::string_view to_string(Mode m);
std::optional<Mode> parse_mode(std::string_view s);

struct InterconnectConfig {
    std::string topology = "ring";
    int link_latency = 1;
    // Project3 inheritance: link_width_log2 = 3 (8-byte links),
    // block_size_log2 = 6 (64-byte cache lines). Header / payload flit counts
    // are derived at Network construction:
    //   header_flits  = 1 << (4 - link_width_log2)
    //   payload_flits = 1 << (block_size_log2 - link_width_log2)
    int link_width_log2 = 3;
    int block_size_log2 = 6;
};

struct MemoryConfig {
    int latency = 100;
    int block_size = 64;
};

struct PredictorConfig {
    std::string type = "yeh_patt";
    int history_bits = 10;             // Yeh-Patt H
    int pattern_bits = 5;              // Yeh-Patt P
    int perceptron_history_bits = 9;   // Perceptron G (global history register width)
    int perceptron_index_bits   = 7;   // Perceptron N (perceptron table = 2^N)
    int hybrid_init             = 2;   // Tournament initial state, encoded 0..3 (see hybrid.cpp)
    int tournament_index_bits   = 12;  // Hybrid selector table size = 1 << this
    int tournament_counter_bits = 4;   // Hybrid selector counter width
};

struct CoreConfig {
    int fetch_width = 4;
    int rob_entries = 96;
    int schedq_entries_per_fu = 2;
    int dispq_capacity = 32;
    int alu_fus = 3;
    int mul_fus = 2;
    int lsu_fus = 2;
    int alu_stages = 1;
    int mul_stages = 3;
    int lsu_stages = 1;
    // Abort the OoO loop if no pipeline progress is observed for this many
    // consecutive cycles. 0 disables the watchdog. The default sits well
    // above the longest legitimate stall (a chain of dependent DRAM-bound
    // loads), so a real config should never trip it; lowering it is useful
    // when bisecting a wedge.
    int deadlock_threshold_cycles = 100000;
    PredictorConfig predictor{};
};

struct CacheLevelConfig {
    int size_kb = 32;
    int block_size = 64;
    int assoc = 8;
    std::string replacement = "lru";
    std::string write_policy = "writeback";
    std::string prefetcher = "none";
    int hit_latency = 2;
    int n_markov_rows = 0; // only used by markov / hybrid prefetchers
    int mshr_entries = 8;  // bounds in-flight misses from the OoO core
};

struct CoherenceConfig {
    std::string protocol = "mesi";
    // Cache-hierarchy topology. "private_l2" is the original Phase 5B
    // design (per-core L1 + per-core L2; directory on its own ring node).
    // "shared_lls" is the Phase 6 hybrid: per-core L1, single shared
    // last-level cache (Last-Level Shared) with the directory embedded
    // in it. Phase 1 only adds the schema; the shared_lls data path is
    // built in Phase 2+.
    std::string cache_mode = "private_l2";
    // Inclusion policy for the shared LLS, when cache_mode = "shared_lls".
    // Inclusive: every line in any L1 is also in the LLS, so an LLS miss
    // means no L1 has it (snoop filter for free). v0 only supports
    // inclusive; "non_inclusive" is reserved for a follow-up.
    std::string inclusion = "inclusive";
};

struct SimConfig {
    int cores = 4;
    InterconnectConfig interconnect{};
    MemoryConfig memory{};
    CoreConfig core{};
    CacheLevelConfig l1{};
    CacheLevelConfig l2{};
    // Shared last-level cache config. Read only when
    // coherence.cache_mode = "shared_lls"; ignored otherwise. Default
    // sizes the LLS to roughly the aggregate of 4 private L2s
    // (4 x 256 KB ~= 1 MB) so an apples-to-apples private-vs-shared
    // capacity comparison is the natural baseline.
    CacheLevelConfig lls{
        .size_kb       = 1024,
        .assoc         = 16,
        .prefetcher    = "markov",
        .hit_latency   = 10,
        .n_markov_rows = 64,
        .mshr_entries  = 32,
    };
    // Top-level predictor block read by --mode predictor. Mirrors the
    // top-level l1/l2 blocks. core.predictor remains the per-core slot
    // used by Phase 4's OoO core.
    PredictorConfig predictor{};
    CoherenceConfig coherence{};
};

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

SimConfig load_config(const std::filesystem::path& path);
std::string dump_config(const SimConfig& cfg, int indent = 2);

void to_json(nlohmann::json& j, const InterconnectConfig& v);
void from_json(const nlohmann::json& j, InterconnectConfig& v);
void to_json(nlohmann::json& j, const MemoryConfig& v);
void from_json(const nlohmann::json& j, MemoryConfig& v);
void to_json(nlohmann::json& j, const PredictorConfig& v);
void from_json(const nlohmann::json& j, PredictorConfig& v);
void to_json(nlohmann::json& j, const CoreConfig& v);
void from_json(const nlohmann::json& j, CoreConfig& v);
void to_json(nlohmann::json& j, const CacheLevelConfig& v);
void from_json(const nlohmann::json& j, CacheLevelConfig& v);
void to_json(nlohmann::json& j, const CoherenceConfig& v);
void from_json(const nlohmann::json& j, CoherenceConfig& v);
void to_json(nlohmann::json& j, const SimConfig& v);
void from_json(const nlohmann::json& j, SimConfig& v);

} // namespace comparch
