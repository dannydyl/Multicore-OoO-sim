#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include "comparch/config.hpp"

TEST_CASE("SimConfig JSON round-trip preserves all fields", "[config]") {
    comparch::SimConfig orig;
    orig.cores = 8;
    orig.interconnect.topology = "xbar";
    orig.interconnect.link_latency = 3;
    orig.interconnect.link_width_log2 = 4;
    orig.interconnect.block_size_log2 = 7;
    orig.memory.latency = 200;
    orig.memory.block_size = 128;
    orig.core.fetch_width = 6;
    orig.core.rob_entries = 128;
    orig.core.predictor.type = "perceptron";
    orig.core.predictor.history_bits = 14;
    orig.l1.size_kb = 64;
    orig.l1.replacement = "lip";
    orig.l1.prefetcher = "stride";
    orig.l2.size_kb = 1024;
    orig.l2.assoc = 16;
    orig.coherence.protocol = "moesif";

    nlohmann::json j = orig;
    auto rt = j.get<comparch::SimConfig>();

    REQUIRE(rt.cores == 8);
    REQUIRE(rt.interconnect.topology == "xbar");
    REQUIRE(rt.interconnect.link_latency == 3);
    REQUIRE(rt.interconnect.link_width_log2 == 4);
    REQUIRE(rt.interconnect.block_size_log2 == 7);
    REQUIRE(rt.memory.latency == 200);
    REQUIRE(rt.memory.block_size == 128);
    REQUIRE(rt.core.fetch_width == 6);
    REQUIRE(rt.core.rob_entries == 128);
    REQUIRE(rt.core.predictor.type == "perceptron");
    REQUIRE(rt.core.predictor.history_bits == 14);
    REQUIRE(rt.l1.size_kb == 64);
    REQUIRE(rt.l1.replacement == "lip");
    REQUIRE(rt.l1.prefetcher == "stride");
    REQUIRE(rt.l2.size_kb == 1024);
    REQUIRE(rt.l2.assoc == 16);
    REQUIRE(rt.coherence.protocol == "moesif");
}

TEST_CASE("Loading configs/baseline.json succeeds with expected values", "[config]") {
    const auto cfg = comparch::load_config(BASELINE_CONFIG_PATH);

    REQUIRE(cfg.cores >= 1);
    REQUIRE(cfg.coherence.protocol == "mesi");
    REQUIRE(cfg.l1.block_size == 64);
    REQUIRE(cfg.l2.size_kb > cfg.l1.size_kb);
    REQUIRE(cfg.core.predictor.type == "yeh_patt");
    REQUIRE(cfg.interconnect.link_width_log2 == 3);   // 8-byte links
    REQUIRE(cfg.interconnect.block_size_log2 == 6);   // 64-byte blocks
}

TEST_CASE("load_config throws ConfigError for missing file", "[config]") {
    REQUIRE_THROWS_AS(
        comparch::load_config("/nonexistent/path/that/should/not/exist.json"),
        comparch::ConfigError);
}

TEST_CASE("Mode round-trips through to_string and parse_mode", "[config]") {
    using comparch::Mode;
    for (auto m : {Mode::Full, Mode::Cache, Mode::Predictor, Mode::Ooo, Mode::Coherence}) {
        const auto s = comparch::to_string(m);
        const auto parsed = comparch::parse_mode(s);
        REQUIRE(parsed.has_value());
        REQUIRE(*parsed == m);
    }
    REQUIRE_FALSE(comparch::parse_mode("nonsense").has_value());
}
