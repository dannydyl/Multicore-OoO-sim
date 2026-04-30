// proj3 regression. Builds an N-CPU coherent ring, runs each per-core
// trace under each ported protocol, and asserts the stdout block
// matches project3's reference output byte-for-byte (modulo the
// `Trace Directory:` line, which echoes whatever path the user
// passes — we strip it before comparing).
//
// Steps 5-8 add MESI / MOSI / MOESIF; this file currently exercises
// MSI only.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include "comparch/coherence/coherence_mode.hpp"
#include "comparch/config.hpp"

namespace fs = std::filesystem;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Drop the "\tTrace Directory: ..." line (the one variable line in the
// banner) so we can compare absolute vs. relative trace dir runs.
std::string strip_trace_dir(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t pos = 0;
    while (pos < in.size()) {
        const auto eol = in.find('\n', pos);
        const auto end = (eol == std::string_view::npos) ? in.size() : eol + 1;
        const std::string_view line = in.substr(pos, end - pos);
        if (line.find("Trace Directory:") == std::string_view::npos) {
            out.append(line);
        }
        pos = end;
    }
    return out;
}

// RAII redirect of std::cout to an in-memory buffer.
class CoutCapture {
public:
    CoutCapture() : prev_(std::cout.rdbuf(buf_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(prev_); }
    CoutCapture(const CoutCapture&) = delete;
    CoutCapture& operator=(const CoutCapture&) = delete;
    std::string str() const { return buf_.str(); }
private:
    std::stringstream  buf_;
    std::streambuf*    prev_;
};

void run_one(const std::string& protocol, int cores) {
    const fs::path fixtures = PROJ3_FIXTURE_DIR;
    const fs::path trace_dir = fixtures / "traces" / ("core_" + std::to_string(cores));
    const fs::path ref_out   =
        fixtures / "ref_outs" / (protocol + "_core_" + std::to_string(cores) + ".out");

    REQUIRE(fs::is_directory(trace_dir));
    REQUIRE(fs::is_regular_file(ref_out));

    comparch::SimConfig cfg;
    cfg.cores = cores;
    cfg.coherence.protocol = [&]() {
        std::string p = protocol;
        for (auto& c : p) c = static_cast<char>(std::tolower(c));
        return p;
    }();
    // Rest of cfg uses InterconnectConfig / MemoryConfig defaults, which
    // already match project3 (link_width_log2=3, block_size_log2=6,
    // mem_latency=100).

    comparch::CliArgs cli;
    cli.trace_dir = trace_dir;
    cli.mode = comparch::Mode::Coherence;

    std::string captured;
    {
        CoutCapture cap;
        const int rc = comparch::coherence::run_coherence_mode(cfg, cli);
        REQUIRE(rc == 0);
        captured = cap.str();
    }

    const std::string expected = slurp(ref_out);
    REQUIRE(strip_trace_dir(captured) == strip_trace_dir(expected));
}

} // namespace

TEST_CASE("proj3 regression: MSI matches project3 reference output",
          "[coherence][proj3-regression][msi]") {
    const int cores = GENERATE(4, 8, 12, 16);
    run_one("MSI", cores);
}

TEST_CASE("proj3 regression: MESI matches project3 reference output",
          "[coherence][proj3-regression][mesi]") {
    const int cores = GENERATE(4, 8, 12, 16);
    run_one("MESI", cores);
}

TEST_CASE("proj3 regression: MOSI matches project3 reference output",
          "[coherence][proj3-regression][mosi]") {
    const int cores = GENERATE(4, 8, 12, 16);
    run_one("MOSI", cores);
}

TEST_CASE("proj3 regression: MOESIF matches project3 reference output",
          "[coherence][proj3-regression][moesif]") {
    const int cores = GENERATE(4, 8, 12, 16);
    run_one("MOESIF", cores);
}
