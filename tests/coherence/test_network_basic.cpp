// Structural tests for the Phase 5A coherence scaffolding (no protocol
// agents wired). Verifies that an empty per-core trace dir reaches
// is_done() at construction, that Message ctor sizes flits per the
// settings, and that Network ticks safely with a null agent factory.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "comparch/coherence/coherence_stats.hpp"
#include "comparch/coherence/message.hpp"
#include "comparch/coherence/network.hpp"
#include "comparch/coherence/settings.hpp"

using namespace comparch::coherence;

namespace {

Settings default_settings(NodeId num_procs) {
    Settings s;
    s.protocol = Protocol::MSI;
    s.num_procs = num_procs;
    s.mem_latency = 100;
    s.block_size_log2 = 6;
    s.link_width_log2 = 3;
    finalize_settings(s);
    return s;
}

std::filesystem::path make_empty_trace_dir(NodeId num_procs,
                                           const std::string& tag) {
    auto root = std::filesystem::temp_directory_path() / ("coh_empty_" + tag);
    std::filesystem::create_directories(root);
    for (NodeId i = 0; i < num_procs; ++i) {
        std::ofstream(root / ("p" + std::to_string(i) + ".trace"));
    }
    return root;
}

} // namespace

TEST_CASE("finalize_settings derives header / payload flit counts",
          "[coherence][settings]") {
    Settings s = default_settings(4);
    REQUIRE(s.header_flits  == 2);   // 1 << (4 - 3)
    REQUIRE(s.payload_flits == 8);   // 1 << (6 - 3)
}

TEST_CASE("Message ctor adds payload flits only for data messages",
          "[coherence][message]") {
    Settings s = default_settings(4);
    Message getm(1, 2, 0x40, MessageKind::GETM, s);
    Message data(1, 2, 0x40, MessageKind::DATA, s);
    Message data_e(1, 2, 0x40, MessageKind::DATA_E, s);
    Message data_f(1, 2, 0x40, MessageKind::DATA_F, s);
    Message inv(1, 2, 0x40, MessageKind::REQ_INVALID, s);

    REQUIRE(getm.flits  == 2);  // header only
    REQUIRE(data.flits  == 10); // header + payload
    REQUIRE(data_e.flits == 10);
    REQUIRE(data_f.flits == 10);
    REQUIRE(inv.flits   == 2);  // header only
}

TEST_CASE("Network with empty traces and null factory reaches is_done immediately",
          "[coherence][network]") {
    const Settings s = default_settings(4);
    const auto trace_dir = make_empty_trace_dir(s.num_procs, "n4");

    CoherenceStats stats;
    Network net(s, stats, trace_dir, /*agent_factory=*/{});

    REQUIRE(net.num_nodes() == s.num_procs + 1);
    REQUIRE(net.is_done());
    REQUIRE(stats.cycles == 0);
}

TEST_CASE("Network::tick / tock are safe to call with empty traces",
          "[coherence][network]") {
    const Settings s = default_settings(2);
    const auto trace_dir = make_empty_trace_dir(s.num_procs, "n2");

    CoherenceStats stats;
    Network net(s, stats, trace_dir, /*agent_factory=*/{});

    Timestamp clock = 0;
    for (int i = 0; i < 3; ++i) {
        net.tick(clock);
        net.tock();
        ++clock;
    }
    REQUIRE(net.is_done());
}
