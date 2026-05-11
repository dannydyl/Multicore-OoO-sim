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

    // Queue a writeback from `src`. The source-side dirty flag is
    // what the real adapter would put on the message (mirrors the
    // L1 line's actual dirty bit at eviction time) and is what
    // handle_writeback uses to bump memory_writes — match the
    // entry's initial_dirty here so the tests describe the same
    // scenario the production code produces.
    auto* msg = new Message(src, num_procs, /*block=*/(0x40 >> 6), kind, s);
    if (kind == MessageKind::DATA_WB) msg->dirty = initial_dirty;
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

// A.3 regression: prior bug was that memory_writes was inferred from
// the directory's tracked state, which can already have transitioned
// off M/O/F by the time the WB drains. With the source-side dirty
// flag on the message, memory_writes counts correctly even when the
// directory thinks the line is in S at the time the WB is processed.
TEST_CASE("Dirty WB still bumps memory_writes when dir state has moved off M",
          "[coherence][writeback][a3]") {
    CoherenceStats stats;
    // Simulate the race: source 2 evicted a dirty M line, but by the
    // time the WB lands the directory has transitioned to S (e.g.
    // because another node's GETS was processed first and the line
    // was already being shared back). The line's actual data was
    // dirty on the source — memory_writes must still fire.
    const auto entry = drive_one(
        Protocol::MESI, /*src=*/2, MessageKind::DATA_WB,
        DirState::S, /*dirty=*/true, /*presence_src=*/true,
        /*o_f_id=*/static_cast<NodeId>(-1), stats);

    REQUIRE(stats.memory_writes == 1);
    REQUIRE_FALSE(entry.presence[2]);
}

TEST_CASE("Clean WB does not bump memory_writes even from former M holder",
          "[coherence][writeback][a3]") {
    CoherenceStats stats;
    // Symmetric: the line was clean at eviction (e.g. a clean S
    // share got dropped). The directory might still think we're
    // in M (rare, but possible during a transient race). Without
    // the source-side flag we'd have over-counted; with it we
    // correctly skip the memory write.
    const auto entry = drive_one(
        Protocol::MESI, /*src=*/0, MessageKind::DATA_WB,
        DirState::M, /*dirty=*/false, /*presence_src=*/true,
        /*o_f_id=*/static_cast<NodeId>(-1), stats);
    (void)entry;

    REQUIRE(stats.memory_writes == 0);
}
