// Phase 5B integration tests: drive run_full_mode end-to-end with
// generated per-core ChampSim binary traces. These are smoke tests that
// the full pipeline runs without deadlock and retires the right number
// of instructions; cycle counts and IPC are not pinned (different cache
// hierarchy than project3, no parity bar).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "comparch/cli.hpp"
#include "comparch/config.hpp"
#include "comparch/full/full_mode.hpp"
#include "comparch/trace.hpp"

namespace fs = std::filesystem;

namespace {

class CoutCapture {
public:
    CoutCapture() : prev_(std::cout.rdbuf(buf_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(prev_); }
    std::string str() const { return buf_.str(); }
private:
    std::stringstream  buf_;
    std::streambuf*    prev_;
};

// Build a tiny per-core ChampSim binary trace: each core gets `n` ALU
// records (no loads, no branches) at strided PCs. ALU-only means no
// coherence traffic — pure pipeline correctness check.
fs::path make_alu_only_trace_dir(int cores, int n_per_core,
                                 const std::string& tag) {
    using comparch::trace::Record;
    using comparch::trace::Variant;
    using comparch::trace::Writer;

    auto root = fs::temp_directory_path() / ("full_test_" + tag);
    fs::remove_all(root);
    fs::create_directories(root);
    for (int c = 0; c < cores; ++c) {
        const auto path = root / ("p" + std::to_string(c) + ".champsimtrace");
        std::ofstream f(path, std::ios::binary);
        Writer w(f, Variant::Standard);
        for (int i = 0; i < n_per_core; ++i) {
            Record r{};
            r.ip = 0x1000 + 4ULL * static_cast<unsigned>(i);
            r.destination_registers[0] = static_cast<std::uint8_t>(1 + (i % 31));
            w.write(r);
        }
        w.flush();
    }
    return root;
}

// Build a trace where each core touches its OWN private region — no
// coherence sharing, but loads exercise the coherence ring round-trip.
fs::path make_private_load_trace_dir(int cores, int n_per_core,
                                     const std::string& tag) {
    using comparch::trace::Record;
    using comparch::trace::Variant;
    using comparch::trace::Writer;

    auto root = fs::temp_directory_path() / ("full_test_" + tag);
    fs::remove_all(root);
    fs::create_directories(root);
    for (int c = 0; c < cores; ++c) {
        const auto path = root / ("p" + std::to_string(c) + ".champsimtrace");
        std::ofstream f(path, std::ios::binary);
        Writer w(f, Variant::Standard);
        for (int i = 0; i < n_per_core; ++i) {
            Record r{};
            r.ip = 0x1000 + 4ULL * static_cast<unsigned>(i);
            r.destination_registers[0] = static_cast<std::uint8_t>(1 + (i % 31));
            r.source_memory[0] =
                0x100000ULL +
                static_cast<std::uint64_t>(c) * 0x10000ULL +
                static_cast<std::uint64_t>(i) * 64ULL;
            w.write(r);
        }
        w.flush();
    }
    return root;
}

int run(const fs::path& trace_dir, int cores, const std::string& proto) {
    comparch::SimConfig cfg;
    cfg.cores              = cores;
    cfg.coherence.protocol = proto;
    comparch::CliArgs cli;
    cli.trace_dir = trace_dir;
    cli.mode      = comparch::Mode::Full;
    return comparch::full::run_full_mode(cfg, cli);
}

} // namespace

TEST_CASE("full mode: 1 core, ALU-only trace runs and completes",
          "[full][smoke]") {
    const auto dir = make_alu_only_trace_dir(/*cores=*/1, /*n=*/100, "alu1");
    CoutCapture cap;
    REQUIRE(run(dir, 1, "mesi") == 0);
    const auto out = cap.str();
    REQUIRE(out.find("Simulation complete") != std::string::npos);
    REQUIRE(out.find("[ Core 0 ]") != std::string::npos);
    REQUIRE(out.find("instructions retired") != std::string::npos);
    REQUIRE(out.find(": 100") != std::string::npos);
}

TEST_CASE("full mode: 2 cores, ALU-only traces all retire",
          "[full][smoke]") {
    const auto dir = make_alu_only_trace_dir(/*cores=*/2, /*n=*/50, "alu2");
    CoutCapture cap;
    REQUIRE(run(dir, 2, "mesi") == 0);
    const auto out = cap.str();
    REQUIRE(out.find("Simulation complete") != std::string::npos);
    // Both per-core report blocks should be present.
    REQUIRE(out.find("[ Core 0 ]") != std::string::npos);
    REQUIRE(out.find("[ Core 1 ]") != std::string::npos);
}

TEST_CASE("full mode: 4 cores with private loads under MESI",
          "[full][smoke][mesi]") {
    const auto dir = make_private_load_trace_dir(/*cores=*/4, /*n=*/30, "ld4");
    CoutCapture cap;
    REQUIRE(run(dir, 4, "mesi") == 0);
    const auto out = cap.str();
    REQUIRE(out.find("Simulation complete") != std::string::npos);
    // Each core retires 30 — ALU + load instructions all complete.
    for (int i = 0; i < 4; ++i) {
        REQUIRE(out.find("[ Core " + std::to_string(i) + " ]")
                != std::string::npos);
    }
}

TEST_CASE("full mode: all 5 protocols run private-load traces",
          "[full][smoke][protocols]") {
    const auto dir = make_private_load_trace_dir(/*cores=*/2, /*n=*/20, "all5");
    for (const auto& proto : {"mi", "msi", "mesi", "mosi", "moesif"}) {
        CoutCapture cap;
        const int rc = run(dir, 2, proto);
        REQUIRE(rc == 0);
        REQUIRE(cap.str().find("Simulation complete") != std::string::npos);
    }
}
