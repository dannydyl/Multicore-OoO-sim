// Unit tests for the MSHR module in isolation. The Cache integration is
// covered separately by test_cache_basic.cpp's handle-based cases.

#include <catch2/catch_test_macros.hpp>

#include "comparch/cache/mshr.hpp"

using comparch::cache::AccessResult;
using comparch::cache::MSHR;
using comparch::cache::MSHREntry;
using comparch::cache::Op;

namespace {

AccessResult miss_result(unsigned latency) {
    AccessResult r;
    r.hit     = false;
    r.latency = latency;
    return r;
}

AccessResult hit_result(unsigned latency) {
    AccessResult r;
    r.hit     = true;
    r.latency = latency;
    return r;
}

} // namespace

TEST_CASE("MSHR allocate-tick-ready timeline", "[mshr]") {
    MSHR mshr(4);
    REQUIRE(mshr.capacity() == 4);
    REQUIRE(mshr.occupancy() == 0);
    REQUIRE_FALSE(mshr.full());

    // Allocate one entry at cycle 0 due in 100 cycles.
    auto* e = mshr.allocate(/*id=*/1, /*block=*/0x80, Op::Read,
                            /*pc=*/0xCAFE, /*due_cycle=*/100,
                            miss_result(100));
    REQUIRE(e != nullptr);
    REQUIRE(e->id == 1);
    REQUIRE(e->valid);
    REQUIRE_FALSE(e->ready);
    REQUIRE(e->due_cycle == 100);

    // Tick up to but not past due_cycle.
    mshr.tick(99);
    REQUIRE_FALSE(mshr.find(1)->ready);

    // Tick past due_cycle: ready flips.
    mshr.tick(100);
    REQUIRE(mshr.find(1)->ready);
}

TEST_CASE("MSHR find returns null for unknown ids", "[mshr]") {
    MSHR mshr(2);
    mshr.allocate(7, 0x40, Op::Read, 0, 10, miss_result(10));
    REQUIRE(mshr.find(7) != nullptr);
    REQUIRE(mshr.find(99) == nullptr);
    REQUIRE(static_cast<const MSHR&>(mshr).find(99) == nullptr);
}

TEST_CASE("MSHR full() returns nullptr from allocate", "[mshr]") {
    MSHR mshr(2);
    REQUIRE(mshr.allocate(1, 0x40, Op::Read, 0, 10, miss_result(10)) != nullptr);
    REQUIRE(mshr.allocate(2, 0x80, Op::Read, 0, 10, miss_result(10)) != nullptr);
    REQUIRE(mshr.full());
    // Third allocation to a *new* block has no slot and no merge candidate.
    REQUIRE(mshr.allocate(3, 0xC0, Op::Read, 0, 10, miss_result(10)) == nullptr);
}

TEST_CASE("MSHR merges secondary requests onto existing block", "[mshr][merge]") {
    MSHR mshr(2);
    auto* primary = mshr.allocate(1, 0x40, Op::Read, 0, 100, miss_result(100));
    REQUIRE(primary != nullptr);
    REQUIRE(primary->merged_ids.empty());

    // Same-block secondary: piggybacks; due_cycle / result inherited from primary.
    auto* secondary = mshr.allocate(2, 0x40, Op::Read, 0, /*ignored=*/999,
                                    /*ignored=*/hit_result(999));
    REQUIRE(secondary == primary);                // same slot
    REQUIRE(secondary->merged_ids.size() == 1);
    REQUIRE(secondary->merged_ids[0] == 2);
    REQUIRE(secondary->due_cycle == 100);          // primary's timing kept
    REQUIRE(mshr.occupancy() == 1);                // still just one slot

    // Both ids resolve to the same entry.
    REQUIRE(mshr.find(1) == primary);
    REQUIRE(mshr.find(2) == primary);

    // After tick, both observe ready simultaneously.
    mshr.tick(100);
    REQUIRE(mshr.find(1)->ready);
    REQUIRE(mshr.find(2)->ready);
}

TEST_CASE("MSHR release frees the slot when no holders remain", "[mshr]") {
    MSHR mshr(2);
    mshr.allocate(1, 0x40, Op::Read, 0, 10, miss_result(10));
    REQUIRE(mshr.occupancy() == 1);

    mshr.release(1);
    REQUIRE(mshr.occupancy() == 0);
    REQUIRE(mshr.find(1) == nullptr);
}

TEST_CASE("MSHR release-then-allocate reuses the slot", "[mshr]") {
    MSHR mshr(1);
    mshr.allocate(1, 0x40, Op::Read, 0, 10, miss_result(10));
    REQUIRE(mshr.full());
    mshr.release(1);
    REQUIRE_FALSE(mshr.full());

    auto* e = mshr.allocate(2, 0xC0, Op::Read, 0, 20, miss_result(20));
    REQUIRE(e != nullptr);
    REQUIRE(e->id == 2);
    REQUIRE(e->block_addr == 0xC0);
}

TEST_CASE("MSHR release of secondary keeps primary live", "[mshr][merge]") {
    MSHR mshr(1);
    auto* primary = mshr.allocate(1, 0x40, Op::Read, 0, 100, miss_result(100));
    mshr.allocate(2, 0x40, Op::Read, 0, 0, miss_result(0));    // merged
    mshr.allocate(3, 0x40, Op::Read, 0, 0, miss_result(0));    // merged
    REQUIRE(primary->merged_ids.size() == 2);

    // Releasing a secondary just trims the merged_ids list; primary stays.
    mshr.release(2);
    REQUIRE(mshr.find(1) == primary);
    REQUIRE(mshr.find(2) == nullptr);
    REQUIRE(mshr.find(3) == primary);
    REQUIRE(primary->merged_ids.size() == 1);
    REQUIRE(primary->merged_ids[0] == 3);
}

TEST_CASE("MSHR release of primary promotes a secondary", "[mshr][merge]") {
    MSHR mshr(1);
    mshr.allocate(1, 0x40, Op::Read, 0, 100, miss_result(100));
    mshr.allocate(7, 0x40, Op::Read, 0, 0, miss_result(0));     // merged

    // The primary releases first. The slot must stay live so the secondary
    // can still find it; the secondary is promoted to primary.
    mshr.release(1);
    REQUIRE(mshr.find(1) == nullptr);
    auto* now = mshr.find(7);
    REQUIRE(now != nullptr);
    REQUIRE(now->id == 7);
    REQUIRE(now->merged_ids.empty());
    REQUIRE(now->valid);

    // Releasing the (newly promoted) primary frees the slot.
    mshr.release(7);
    REQUIRE(mshr.find(7) == nullptr);
    REQUIRE(mshr.occupancy() == 0);
}

TEST_CASE("MSHR release of unknown id is a no-op", "[mshr]") {
    MSHR mshr(2);
    mshr.allocate(1, 0x40, Op::Read, 0, 10, miss_result(10));
    REQUIRE(mshr.occupancy() == 1);
    mshr.release(/*unknown=*/42);
    REQUIRE(mshr.occupancy() == 1);
    REQUIRE(mshr.find(1) != nullptr);
}

TEST_CASE("MSHR tick is monotone — entries don't unflip", "[mshr]") {
    MSHR mshr(1);
    mshr.allocate(1, 0x40, Op::Read, 0, 10, miss_result(10));
    mshr.tick(10);
    REQUIRE(mshr.find(1)->ready);
    // Calling tick with a smaller `now` shouldn't matter (Cache always
    // advances `now_` monotonically), but if it ever happens, ready stays.
    mshr.tick(5);
    REQUIRE(mshr.find(1)->ready);
}
