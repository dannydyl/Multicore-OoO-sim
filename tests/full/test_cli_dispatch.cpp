// Phase 5B CLI smoke tests: confirm "full" is no longer a valid --mode
// keyword, default invocation routes to Mode::Full, and parse_mode("full")
// still works internally for round-trip tests.

#include <catch2/catch_test_macros.hpp>

#include <cstring>

#include "comparch/cli.hpp"
#include "comparch/config.hpp"

constexpr std::size_t BASELINE_CONFIG_PATH_LEN = sizeof(BASELINE_CONFIG_PATH) - 1;

TEST_CASE("Mode::Full is the CliArgs default", "[full][cli]") {
    comparch::CliArgs args;
    REQUIRE(args.mode == comparch::Mode::Full);
}

TEST_CASE("parse_mode('full') still resolves internally",
          "[full][cli]") {
    auto m = comparch::parse_mode("full");
    REQUIRE(m.has_value());
    REQUIRE(*m == comparch::Mode::Full);
}

TEST_CASE("to_string(Mode::Full) round-trips through parse_mode",
          "[full][cli]") {
    const auto s = comparch::to_string(comparch::Mode::Full);
    auto m = comparch::parse_mode(s);
    REQUIRE(m.has_value());
    REQUIRE(*m == comparch::Mode::Full);
}

TEST_CASE("CLI parser: --mode full is rejected with exit 1",
          "[full][cli]") {
    char arg0[]   = "sim";
    char arg1[]   = "--config";
    char arg2[BASELINE_CONFIG_PATH_LEN + 1];
    std::strcpy(arg2, BASELINE_CONFIG_PATH);
    char arg3[]   = "--mode";
    char arg4[]   = "full";
    char* argv[]  = {arg0, arg1, arg2, arg3, arg4, nullptr};
    auto r = comparch::parse_cli(5, argv);
    REQUIRE(r.should_exit);
    REQUIRE(r.exit_code == 1);
}

TEST_CASE("CLI parser: no --mode -> CliArgs.mode stays Mode::Full",
          "[full][cli]") {
    char arg0[]  = "sim";
    char arg1[]  = "--config";
    char arg2[BASELINE_CONFIG_PATH_LEN + 1];
    std::strcpy(arg2, BASELINE_CONFIG_PATH);
    char* argv[] = {arg0, arg1, arg2, nullptr};
    auto r = comparch::parse_cli(3, argv);
    REQUIRE_FALSE(r.should_exit);
    REQUIRE(r.args.mode == comparch::Mode::Full);
}
