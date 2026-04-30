// Pins the project3-compatible banner / stats format byte-for-byte.
// Any drift breaks the proj3 regression test, so isolate format-string
// changes here first.

#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/config.hpp"

using comparch::SimConfig;
using comparch::coherence::CoherenceStats;
using comparch::coherence::print_banner;
using comparch::coherence::print_stats;
using comparch::coherence::proto_label;

namespace {

SimConfig msi_core_4_cfg() {
    SimConfig c;
    c.cores = 4;
    c.coherence.protocol = "msi";
    c.interconnect.topology = "ring";
    c.interconnect.link_width_log2 = 3;   // -> "8 bytes"
    c.interconnect.block_size_log2 = 6;   // -> "6"
    c.memory.latency = 100;
    return c;
}

} // namespace

TEST_CASE("proto_label maps protocol strings to legacy proto_str entries",
          "[coherence][stats]") {
    REQUIRE(proto_label("mi")     == "MI_PRO");
    REQUIRE(proto_label("msi")    == "MSI_PRO");
    REQUIRE(proto_label("mesi")   == "MESI_PRO");
    REQUIRE(proto_label("mosi")   == "MOSI_PRO");
    REQUIRE(proto_label("moesif") == "MOESIF_PRO");
    REQUIRE_THROWS_AS(proto_label("nonsense"), comparch::ConfigError);
}

TEST_CASE("print_banner matches project3 MSI core_4 banner verbatim",
          "[coherence][stats]") {
    // Pinned against project3_v1.1.0/ref_outs/MSI_core_4.out (lines 1-9).
    const std::string expected =
        "Selected Configuration:\n"
        "\tProtocol: MSI_PRO\n"
        "\tTrace Directory: traces/core_4\n"
        "\tNum Procs: 4\n"
        "\tCPU TYPE: FICI_CPU\n"
        "\tNetwork Topology: RING_TOP\n"
        "\tLink Width: 8 bytes\n"
        "\tMemory Latency: 100\n"
        "\tBlock Size: 6\n";

    std::ostringstream os;
    print_banner(os, msi_core_4_cfg(), "traces/core_4");
    REQUIRE(os.str() == expected);
}

TEST_CASE("print_stats matches project3 MSI core_4 stats verbatim",
          "[coherence][stats]") {
    // Pinned against project3_v1.1.0/ref_outs/MSI_core_4.out (lines 10-17).
    const std::string expected =
        "Simulation complete\n"
        "Cycles: 133726\n"
        "Cache Misses:         1478 misses\n"
        "Cache Accesses:       1912 accesses\n"
        "Silent Upgrades:         0 upgrades\n"
        "$-to-$ Transfers:      212 transfers\n"
        "Memory Reads:         1268 reads\n"
        "Memory Writes:         209 writes\n";

    CoherenceStats s;
    s.cycles          = 133726;
    s.cache_misses    = 1478;
    s.cache_accesses  = 1912;
    s.silent_upgrades = 0;
    s.c2c_transfers   = 212;
    s.memory_reads    = 1268;
    s.memory_writes   = 209;

    std::ostringstream os;
    print_stats(os, s);
    REQUIRE(os.str() == expected);
}

TEST_CASE("print_stats handles all-zero counters (Phase 5A skeleton path)",
          "[coherence][stats]") {
    CoherenceStats s;     // all zero
    std::ostringstream os;
    print_stats(os, s);

    const std::string expected =
        "Simulation complete\n"
        "Cycles: 0\n"
        "Cache Misses:            0 misses\n"
        "Cache Accesses:          0 accesses\n"
        "Silent Upgrades:         0 upgrades\n"
        "$-to-$ Transfers:        0 transfers\n"
        "Memory Reads:            0 reads\n"
        "Memory Writes:           0 writes\n";
    REQUIRE(os.str() == expected);
}
