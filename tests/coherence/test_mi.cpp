// MI agent + directory unit coverage. Project3 ships no MI reference
// output, so coverage relies on these synthetic scenarios:
//   1. A single CPU loops over private blocks (no inter-core sharing).
//      Counts: every access is a miss in MI's I->IM->M flow because
//      MI lacks an S state, but a re-access of the same block stays
//      in M and is a hit. Verify miss count, c2c=0, mem_writes=0.
//   2. Two CPUs ping-pong on the same block (write/read alternation).
//      Each ownership transfer goes through the directory's MM
//      transient and counts as a $-to-$ transfer.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "comparch/coherence/coherence_mode.hpp"
#include "comparch/config.hpp"

namespace fs = std::filesystem;

namespace {

class CoutCapture {
public:
    CoutCapture() : prev_(std::cout.rdbuf(buf_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(prev_); }
    std::string str() const { return buf_.str(); }
private:
    std::stringstream buf_;
    std::streambuf*   prev_;
};

fs::path write_traces(const std::string& tag,
                      const std::vector<std::string>& per_core_lines) {
    auto root = fs::temp_directory_path() / ("coh_mi_" + tag);
    fs::remove_all(root);
    fs::create_directories(root);
    for (std::size_t i = 0; i < per_core_lines.size(); ++i) {
        std::ofstream f(root / ("p" + std::to_string(i) + ".trace"));
        f << per_core_lines[i];
    }
    return root;
}

comparch::CliArgs cli_for(const fs::path& trace_dir) {
    comparch::CliArgs cli;
    cli.trace_dir = trace_dir;
    cli.mode      = comparch::Mode::Coherence;
    return cli;
}

comparch::SimConfig cfg_for(int cores, const std::string& protocol) {
    comparch::SimConfig cfg;
    cfg.cores              = cores;
    cfg.coherence.protocol = protocol;
    return cfg;
}

// Parses the trailing 7 stat lines emitted by run_coherence_mode and
// returns them as integers. The format is pinned by test_coherence_stats.
struct ParsedStats {
    std::uint64_t cycles = 0;
    std::uint64_t cache_misses = 0;
    std::uint64_t cache_accesses = 0;
    std::uint64_t silent_upgrades = 0;
    std::uint64_t c2c_transfers = 0;
    std::uint64_t memory_reads = 0;
    std::uint64_t memory_writes = 0;
};

std::uint64_t scan_after(const std::string& s, const std::string& label) {
    const auto p = s.find(label);
    REQUIRE(p != std::string::npos);
    std::istringstream iss(s.substr(p + label.size()));
    std::uint64_t v = 0;
    iss >> v;
    return v;
}

ParsedStats parse_stats(const std::string& out) {
    ParsedStats s;
    s.cycles          = scan_after(out, "Cycles: ");
    s.cache_misses    = scan_after(out, "Cache Misses:");
    s.cache_accesses  = scan_after(out, "Cache Accesses:");
    s.silent_upgrades = scan_after(out, "Silent Upgrades:");
    s.c2c_transfers   = scan_after(out, "$-to-$ Transfers:");
    s.memory_reads    = scan_after(out, "Memory Reads:");
    s.memory_writes   = scan_after(out, "Memory Writes:");
    return s;
}

ParsedStats run_mi(const std::vector<std::string>& per_core_lines,
                   const std::string& tag) {
    const auto trace_dir = write_traces(tag, per_core_lines);
    auto cli = cli_for(trace_dir);
    auto cfg = cfg_for(static_cast<int>(per_core_lines.size()), "mi");

    std::string out;
    {
        CoutCapture cap;
        const int rc = comparch::coherence::run_coherence_mode(cfg, cli);
        REQUIRE(rc == 0);
        out = cap.str();
    }
    return parse_stats(out);
}

} // namespace

TEST_CASE("MI: single CPU re-accessing same block stays in M after first miss",
          "[coherence][mi]") {
    // Block 0x40 hit twice: first access miss (I->IM->M), second hit (M).
    // 2 accesses, 1 miss. No c2c, one mem_read, no mem_write.
    const auto s = run_mi({"r 0x40\nr 0x44\n"}, "single_hit");
    REQUIRE(s.cache_accesses == 2);
    REQUIRE(s.cache_misses   == 1);
    REQUIRE(s.c2c_transfers  == 0);
    REQUIRE(s.memory_reads   == 1);
    REQUIRE(s.memory_writes  == 0);
}

TEST_CASE("MI: two CPUs ping-ponging produces $-to-$ transfers",
          "[coherence][mi]") {
    // Both CPUs touch the same block. P0 first miss -> mem read (1).
    // P1 second access on same block -> directory recalls from P0
    // (RECALL_GOTO_I -> DATA -> $-to-$ transfer). One c2c, one mem read.
    const auto s = run_mi({
        "r 0x40\n",
        "r 0x40\n",
    }, "pingpong");
    REQUIRE(s.cache_accesses == 2);
    REQUIRE(s.cache_misses   == 2);   // MI has no S; second core's read also misses
    REQUIRE(s.c2c_transfers  == 1);
    REQUIRE(s.memory_reads   == 1);
}

TEST_CASE("MI: silent_upgrades stays zero (no S->M shortcut path)",
          "[coherence][mi]") {
    const auto s = run_mi({"w 0x80\nw 0x80\nw 0x80\n"}, "writes");
    REQUIRE(s.silent_upgrades == 0);
}
