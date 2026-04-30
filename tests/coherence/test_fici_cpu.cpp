// Trace-parsing tests for the project3-style 'r 0xADDR' / 'w 0xADDR'
// per-core trace format consumed by FiciCpu.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "comparch/coherence/fici_cpu.hpp"
#include "comparch/trace.hpp"

using comparch::coherence::Instruction;
using comparch::coherence::load_proj3_trace;
using comparch::trace::TraceError;

namespace {

std::filesystem::path write_tmp(const std::string& contents,
                                const std::string& tag) {
    auto p = std::filesystem::temp_directory_path() /
             ("test_fici_" + tag + ".trace");
    std::ofstream f(p, std::ios::trunc);
    f << contents;
    f.close();
    return p;
}

} // namespace

TEST_CASE("load_proj3_trace parses canonical lines", "[coherence][fici]") {
    const auto p = write_tmp(
        "r 0x1000\n"
        "w 0xDEADBEEF\n"
        "r 0xabcd\n",
        "canonical");
    const auto v = load_proj3_trace(p);
    REQUIRE(v.size() == 3);
    REQUIRE(v[0].action == 'r');  REQUIRE(v[0].address == 0x1000);
    REQUIRE(v[1].action == 'w');  REQUIRE(v[1].address == 0xDEADBEEFULL);
    REQUIRE(v[2].action == 'r');  REQUIRE(v[2].address == 0xabcdULL);
}

TEST_CASE("load_proj3_trace tolerates trailing blank line",
          "[coherence][fici]") {
    const auto p = write_tmp("r 0x10\n\n", "blank");
    const auto v = load_proj3_trace(p);
    REQUIRE(v.size() == 1);
    REQUIRE(v[0].address == 0x10);
}

TEST_CASE("load_proj3_trace rejects malformed lines",
          "[coherence][fici]") {
    SECTION("unknown opcode") {
        const auto p = write_tmp("x 0x10\n", "badop");
        REQUIRE_THROWS_AS(load_proj3_trace(p), TraceError);
    }
    SECTION("missing address") {
        const auto p = write_tmp("r\n", "noaddr");
        REQUIRE_THROWS_AS(load_proj3_trace(p), TraceError);
    }
    SECTION("non-hex address") {
        const auto p = write_tmp("w hello\n", "nothex");
        REQUIRE_THROWS_AS(load_proj3_trace(p), TraceError);
    }
}

TEST_CASE("load_proj3_trace throws on missing file", "[coherence][fici]") {
    REQUIRE_THROWS_AS(
        load_proj3_trace("/nonexistent/path/that/should/not/exist.trace"),
        TraceError);
}

TEST_CASE("load_proj3_trace returns empty vector on empty file",
          "[coherence][fici]") {
    const auto p = write_tmp("", "empty");
    const auto v = load_proj3_trace(p);
    REQUIRE(v.empty());
}
