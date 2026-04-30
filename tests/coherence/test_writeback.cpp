// Phase 5B: targeted DirectoryController::handle_writeback tests for
// each of the 5 protocols. These verify that an unsolicited DATA_WB
// from a sharer (a) drops the presence bit, (b) increments
// memory_writes only when the dropping core was the dirty owner, and
// (c) collapses the directory state correctly when the last sharer
// leaves.

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "comparch/coherence/directory.hpp"

using namespace comparch::coherence;

namespace {

Settings settings_with(Protocol p, NodeId num_procs = 4) {
    Settings s;
    s.protocol        = p;
    s.num_procs       = num_procs;
    s.mem_latency     = 100;
    s.block_size_log2 = 6;
    s.link_width_log2 = 3;
    finalize_settings(s);
    return s;
}

// Drive one tick on a directory with a single queued message. Returns
// the post-tick state of the per-block entry.
DirEntry drive_one(Protocol proto,
                   NodeId src,
                   MessageKind kind,
                   DirState initial_state,
                   bool initial_dirty,
                   bool initial_presence_src,
                   NodeId initial_o_f_id,
                   CoherenceStats& stats_out,
                   NodeId num_procs = 4) {
    auto s = settings_with(proto, num_procs);
    DirectoryController dir(num_procs, s, stats_out);

    // Pre-populate the block at addr 0x40 with the requested directory state.
    auto* entry = dir.get_entry(/*block=*/(0x40 >> 6));
    entry->state         = initial_state;
    entry->dirty         = initial_dirty;
    entry->presence[src] = initial_presence_src;
    entry->active_sharers = initial_presence_src ? 1u : 0u;
    entry->o_f_id         = initial_o_f_id;

    // Queue a writeback from `src`.
    auto* msg = new Message(src, num_procs, /*block=*/(0x40 >> 6), kind, s);
    dir.request_queue.push_back(msg);

    dir.tick(/*clock=*/1);
    return *dir.get_entry(0x40 >> 6);
}

} // namespace

TEST_CASE("MSI: WRITEBACK from M-holder triggers memory_writes and goes to I",
          "[coherence][writeback][msi]") {
    CoherenceStats stats;
    const auto entry = drive_one(
        Protocol::MSI, /*src=*/2, MessageKind::DATA_WB,
        DirState::M, /*dirty=*/true, /*presence_src=*/true,
        /*o_f_id=*/static_cast<NodeId>(-1), stats);

    REQUIRE(stats.memory_writes == 1);
    REQUIRE(entry.state == DirState::I);
    REQUIRE(entry.active_sharers == 0);
    REQUIRE_FALSE(entry.presence[2]);
}

TEST_CASE("MSI: WRITEBACK from S-sharer drops presence without mem write",
          "[coherence][writeback][msi]") {
    CoherenceStats stats;
    const auto entry = drive_one(
        Protocol::MSI, /*src=*/1, MessageKind::DATA_WB,
        DirState::S, /*dirty=*/false, /*presence_src=*/true,
        /*o_f_id=*/static_cast<NodeId>(-1), stats);

    REQUIRE(stats.memory_writes == 0);
    REQUIRE(entry.state == DirState::I);
    REQUIRE(entry.active_sharers == 0);
}

TEST_CASE("MESI: WRITEBACK from E-holder is a clean drop (no mem write)",
          "[coherence][writeback][mesi]") {
    CoherenceStats stats;
    const auto entry = drive_one(
        Protocol::MESI, /*src=*/0, MessageKind::DATA_WB,
        DirState::E, /*dirty=*/false, /*presence_src=*/true,
        /*o_f_id=*/static_cast<NodeId>(-1), stats);

    REQUIRE(stats.memory_writes == 0);
    REQUIRE(entry.state == DirState::I);
}

TEST_CASE("MOSI: WRITEBACK from O-holder triggers memory_writes",
          "[coherence][writeback][mosi]") {
    CoherenceStats stats;
    const auto entry = drive_one(
        Protocol::MOSI, /*src=*/3, MessageKind::DATA_WB,
        DirState::O, /*dirty=*/true, /*presence_src=*/true,
        /*o_f_id=*/3, stats);

    REQUIRE(stats.memory_writes == 1);
    REQUIRE(entry.active_sharers == 0);
    REQUIRE(entry.state == DirState::I);
}

TEST_CASE("MOESIF: WRITEBACK from F-holder triggers memory_writes",
          "[coherence][writeback][moesif]") {
    CoherenceStats stats;
    const auto entry = drive_one(
        Protocol::MOESIF, /*src=*/1, MessageKind::DATA_WB,
        DirState::F, /*dirty=*/true, /*presence_src=*/true,
        /*o_f_id=*/1, stats);

    REQUIRE(stats.memory_writes == 1);
    REQUIRE(entry.active_sharers == 0);
    REQUIRE(entry.state == DirState::I);
}
