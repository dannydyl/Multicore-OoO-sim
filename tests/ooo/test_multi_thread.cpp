// End-to-end multi-thread fetch-stall test.
//
// Builds two CasimV2 trace streams (one per thread) that take a
// shared lock in a fixed order. Wires both Readers through a single
// SyncCoordinator. Ticks two OoO cores in lockstep and asserts:
//   1) the simulation completes (no deadlock),
//   2) thread 1 was actually blocked at some intermediate cycle,
//   3) the instruction-retire counts match the trace contents.
//
// The point of the test is the OoO core's `if (trace_->blocked()) return;`
// path — it must stall fetch on a sync record that the coordinator
// rejects, and resume once the matching peer event lands.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <sstream>

#include "comparch/cache/cache.hpp"
#include "comparch/ooo/core.hpp"
#include "comparch/ooo/inst.hpp"
#include "comparch/predictor/predictor.hpp"
#include "comparch/sync_coordinator.hpp"
#include "comparch/trace.hpp"

using comparch::cache::Cache;
using comparch::ooo::OooConfig;
using comparch::ooo::OooCore;
using comparch::sync::SyncCoordinator;
using comparch::trace::FileHeader;
using comparch::trace::Reader;
using comparch::trace::Record;
using comparch::trace::SyncKind;
using comparch::trace::SyncRecord;
using comparch::trace::Variant;
using comparch::trace::Writer;

namespace {

Cache::Config small_l1d() {
    Cache::Config c;
    c.c = 12; c.b = 6; c.s = 1;
    c.replacement   = comparch::cache::Replacement::LRU_MIP;
    c.write_policy  = comparch::cache::WritePolicy::WBWA;
    c.hit_latency   = 2;
    c.mshr_entries  = 8;
    return c;
}

OooConfig narrow_4_4() {
    OooConfig c;
    c.fetch_width = 4;
    c.rob_entries = 32;
    c.schedq_entries_per_fu = 4;
    c.alu_fus = 4;
    c.mul_fus = 1;
    c.lsu_fus = 2;
    return c;
}

comparch::PredictorConfig always_taken_cfg() {
    comparch::PredictorConfig p;
    p.type = "always_taken";
    return p;
}

Record alu(std::uint64_t pc) {
    Record r{};
    r.ip = pc;
    return r;
}

// Build an in-memory CasimV2 stream with a header, a vector of
// instructions, and lock_acq/lock_rel(obj=L, seq=...) inserted at
// the requested positions. Returns a stringstream that can back a
// trace::Reader.
std::unique_ptr<std::stringstream> build_v2_stream(
        std::uint32_t tid,
        std::uint32_t total_threads,
        const std::vector<Record>& pre,
        std::uint64_t lock_obj, std::uint64_t acq_seq, std::uint64_t rel_seq,
        const std::vector<Record>& crit,
        const std::vector<Record>& post) {
    auto ss = std::make_unique<std::stringstream>(
        std::ios::in | std::ios::out | std::ios::binary);
    Writer w(*ss, Variant::CasimV2);
    FileHeader h;
    h.thread_id = tid;
    h.thread_count = total_threads;
    h.program_uid = 0xC0FFEE;
    w.write_header(h);
    for (const auto& r : pre)  w.write(r);
    w.write(SyncRecord{SyncKind::LockAcquire, lock_obj, acq_seq, 0, 0, 0});
    for (const auto& r : crit) w.write(r);
    w.write(SyncRecord{SyncKind::LockRelease, lock_obj, rel_seq, 0, 0, 0});
    for (const auto& r : post) w.write(r);
    w.flush();
    return ss;
}

} // namespace

TEST_CASE("Two threads share a lock; thread 1 blocks then unblocks",
          "[ooo][sync][e2e]") {
    constexpr std::uint64_t kLock = 0xCAFE0;

    // Thread 0: 8 ALU pre, take lock seq=0, 12 ALU crit, release seq=0,
    //           4 ALU post. Total = 24 instructions.
    std::vector<Record> t0_pre, t0_crit, t0_post;
    for (int i = 0; i < 8;  ++i) t0_pre.push_back(alu(0x1000 + 4u * i));
    for (int i = 0; i < 12; ++i) t0_crit.push_back(alu(0x1100 + 4u * i));
    for (int i = 0; i < 4;  ++i) t0_post.push_back(alu(0x1200 + 4u * i));

    // Thread 1: 4 ALU pre (much shorter so it reaches its acquire fast,
    // before thread 0 reaches its release), take lock seq=1, 4 crit,
    // release seq=1, 4 post. Total = 12 instructions.
    std::vector<Record> t1_pre, t1_crit, t1_post;
    for (int i = 0; i < 4; ++i) t1_pre.push_back(alu(0x2000 + 4u * i));
    for (int i = 0; i < 4; ++i) t1_crit.push_back(alu(0x2100 + 4u * i));
    for (int i = 0; i < 4; ++i) t1_post.push_back(alu(0x2200 + 4u * i));

    auto s0 = build_v2_stream(0, 2, t0_pre, kLock, 0, 0, t0_crit, t0_post);
    auto s1 = build_v2_stream(1, 2, t1_pre, kLock, 1, 1, t1_crit, t1_post);

    Reader r0(*s0, Variant::CasimV2);
    Reader r1(*s1, Variant::CasimV2);
    SyncCoordinator coord(2);
    r0.set_sync_sink(&coord, /*tid=*/0);
    r1.set_sync_sink(&coord, /*tid=*/1);

    Cache l1d_0(small_l1d(), "L1d#0");
    Cache l1d_1(small_l1d(), "L1d#1");
    auto pred0 = comparch::predictor::make(always_taken_cfg());
    auto pred1 = comparch::predictor::make(always_taken_cfg());

    OooCore c0(narrow_4_4(), *pred0, l1d_0, r0);
    OooCore c1(narrow_4_4(), *pred1, l1d_1, r1);
    c0.set_active_tid(0);
    c1.set_active_tid(1);

    // Tick in lockstep up to a generous cap. The point of the test is
    // that we DON'T deadlock, so a reachable cap is fine; if blocking
    // is wrong we'd run forever and this would fail loudly.
    bool t1_observed_blocked = false;
    constexpr std::uint64_t kCap = 5000;
    std::uint64_t cycles = 0;
    while (cycles < kCap) {
        const bool a = c0.tick();
        const bool b = c1.tick();
        if (coord.is_blocked(1)) t1_observed_blocked = true;
        if (!a && !b) break;
        ++cycles;
    }

    REQUIRE(c0.eof());
    REQUIRE(c1.eof());
    // Each thread retires its ALU records plus 1 LockRelease
    // pseudo-Inst (LockAcquire is a pure gate, dropped at fetch).
    REQUIRE(c0.stats().instructions_retired == 25u);   // 24 ALU + 1 pseudo
    REQUIRE(c1.stats().instructions_retired == 13u);   // 12 ALU + 1 pseudo

    REQUIRE(t1_observed_blocked);
    REQUIRE(coord.stats().sync_stalled  >= 1u);
    REQUIRE(coord.stats().lock_acquires == 2u);
    REQUIRE(coord.stats().lock_releases == 2u);
}

TEST_CASE("Two threads on disjoint locks never block each other",
          "[ooo][sync][e2e]") {
    auto s0 = build_v2_stream(0, 2, {alu(0x10), alu(0x11)},
                              /*lock=*/0xAAA, 0, 0,
                              {alu(0x12)}, {alu(0x13)});
    auto s1 = build_v2_stream(1, 2, {alu(0x20), alu(0x21)},
                              /*lock=*/0xBBB, 0, 0,    // different object
                              {alu(0x22)}, {alu(0x23)});

    Reader r0(*s0, Variant::CasimV2);
    Reader r1(*s1, Variant::CasimV2);
    SyncCoordinator coord(2);
    r0.set_sync_sink(&coord, 0);
    r1.set_sync_sink(&coord, 1);

    Cache l1d_0(small_l1d(), "L1d#0");
    Cache l1d_1(small_l1d(), "L1d#1");
    auto pred0 = comparch::predictor::make(always_taken_cfg());
    auto pred1 = comparch::predictor::make(always_taken_cfg());

    OooCore c0(narrow_4_4(), *pred0, l1d_0, r0);
    OooCore c1(narrow_4_4(), *pred1, l1d_1, r1);
    c0.set_active_tid(0);
    c1.set_active_tid(1);

    constexpr std::uint64_t kCap = 1000;
    std::uint64_t cycles = 0;
    while (cycles < kCap) {
        const bool a = c0.tick();
        const bool b = c1.tick();
        if (!a && !b) break;
        ++cycles;
    }

    REQUIRE(c0.eof());
    REQUIRE(c1.eof());
    // 4 ALU + 1 LockRelease pseudo = 5 retired per thread.
    REQUIRE(c0.stats().instructions_retired == 5u);
    REQUIRE(c1.stats().instructions_retired == 5u);
    // Disjoint locks: neither side should ever stall.
    REQUIRE(coord.stats().sync_stalled == 0u);
}
