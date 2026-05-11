#include <catch2/catch_test_macros.hpp>

#include "comparch/sync_coordinator.hpp"

using comparch::sync::SyncCoordinator;
using comparch::trace::LifecycleKind;
using comparch::trace::LifecycleRecord;
using comparch::trace::SyncKind;
using comparch::trace::SyncRecord;

namespace {

SyncRecord lock_acq(std::uint64_t obj, std::uint64_t seq) {
    return {SyncKind::LockAcquire, obj, seq, 0, 0, 0};
}
SyncRecord lock_rel(std::uint64_t obj, std::uint64_t seq) {
    return {SyncKind::LockRelease, obj, seq, 0, 0, 0};
}
SyncRecord atomic(std::uint64_t obj, std::uint64_t seq) {
    return {SyncKind::AtomicRMW, obj, seq, 0, 0, 0};
}
SyncRecord barrier_arr(std::uint64_t obj, std::uint64_t seq, std::uint64_t expected) {
    return {SyncKind::BarrierArrive, obj, seq, expected, 0, 0};
}
SyncRecord barrier_leave(std::uint64_t obj, std::uint64_t seq) {
    return {SyncKind::BarrierLeave, obj, seq, 0, 0, 0};
}

} // namespace

// Helper: do a full gate+retire cycle for signal-side events.
// Real sim does this in two phases (fetch → retire); these tests
// collapse them since they're not exercising pipeline timing.
namespace {
void release_now(SyncCoordinator& co, std::uint32_t tid, SyncRecord r) {
    REQUIRE(co.try_consume_sync(tid, r));
    co.notify_retire(tid, r);
}
}

TEST_CASE("Lock: first acquire succeeds without prior release", "[sync]") {
    SyncCoordinator co(2);
    REQUIRE(co.try_consume_sync(0, lock_acq(0xABC, 0)));
    REQUIRE(co.is_blocked(0) == false);
    REQUIRE(co.stats().lock_acquires == 1);
}

TEST_CASE("Lock: second acquire stalls until first released at RETIRE", "[sync]") {
    SyncCoordinator co(2);
    REQUIRE(co.try_consume_sync(0, lock_acq(0xABC, 0)));
    // Thread 1 tries to acquire seq=1 before thread 0 releases seq=0.
    REQUIRE_FALSE(co.try_consume_sync(1, lock_acq(0xABC, 1)));
    REQUIRE(co.is_blocked(1));
    REQUIRE(co.stats().sync_stalled == 1);

    // Thread 0's LockRelease passes gate but doesn't yet advance state.
    REQUIRE(co.try_consume_sync(0, lock_rel(0xABC, 0)));
    // Until retire fires, thread 1 still cannot acquire.
    REQUIRE_FALSE(co.try_consume_sync(1, lock_acq(0xABC, 1)));

    // Retire of LockRelease — NOW state advances.
    co.notify_retire(0, lock_rel(0xABC, 0));

    // Thread 1 retries — now succeeds.
    REQUIRE(co.try_consume_sync(1, lock_acq(0xABC, 1)));
    REQUIRE_FALSE(co.is_blocked(1));
}

TEST_CASE("Lock: independent objects don't interfere", "[sync]") {
    SyncCoordinator co(2);
    REQUIRE(co.try_consume_sync(0, lock_acq(0x100, 0)));
    REQUIRE(co.try_consume_sync(1, lock_acq(0x200, 0)));   // different obj
    REQUIRE_FALSE(co.is_blocked(1));
}

TEST_CASE("Lock: out-of-order release beyond max bumps last_completed", "[sync]") {
    SyncCoordinator co(1);
    REQUIRE(co.try_consume_sync(0, lock_acq(0xCAFE, 0)));
    release_now(co, 0, lock_rel(0xCAFE, 0));
    REQUIRE(co.last_completed_seq(0xCAFE) == 0);

    // A stale release at seq=0 (e.g. duplicate) doesn't lower it.
    release_now(co, 0, lock_rel(0xCAFE, 0));
    REQUIRE(co.last_completed_seq(0xCAFE) == 0);

    // A higher release advances.
    REQUIRE(co.try_consume_sync(0, lock_acq(0xCAFE, 1)));
    release_now(co, 0, lock_rel(0xCAFE, 1));
    REQUIRE(co.last_completed_seq(0xCAFE) == 1);
}

TEST_CASE("Atomic: serialization on a single object", "[sync]") {
    SyncCoordinator co(2);
    release_now(co, 0, atomic(0xDEAD, 0));
    release_now(co, 1, atomic(0xDEAD, 1));
    // Out-of-order: seq=3 before seq=2 stalls.
    REQUIRE_FALSE(co.try_consume_sync(0, atomic(0xDEAD, 3)));
    REQUIRE(co.is_blocked(0));
    // Filling in seq=2 then re-trying seq=3 succeeds.
    release_now(co, 1, atomic(0xDEAD, 2));
    release_now(co, 0, atomic(0xDEAD, 3));
}

TEST_CASE("Barrier: Arrive accumulates at RETIRE, Leave gates on count", "[sync]") {
    SyncCoordinator co(4);
    // Three arrivals on a 4-way barrier (gate-pass + retire).
    release_now(co, 0, barrier_arr(0xB0, 0, 4));
    release_now(co, 1, barrier_arr(0xB0, 0, 4));
    release_now(co, 2, barrier_arr(0xB0, 0, 4));

    // Leave with only 3 arrivals: stall.
    REQUIRE_FALSE(co.try_consume_sync(0, barrier_leave(0xB0, 0)));
    REQUIRE(co.is_blocked(0));

    // Fourth arrival.
    release_now(co, 3, barrier_arr(0xB0, 0, 4));
    // Now all four threads can leave.
    REQUIRE(co.try_consume_sync(0, barrier_leave(0xB0, 0)));
    REQUIRE(co.try_consume_sync(1, barrier_leave(0xB0, 0)));
    REQUIRE(co.try_consume_sync(2, barrier_leave(0xB0, 0)));
    REQUIRE(co.try_consume_sync(3, barrier_leave(0xB0, 0)));
    REQUIRE(co.stats().barrier_passes == 4);
}

TEST_CASE("Barrier: separate iterations are tracked separately", "[sync]") {
    SyncCoordinator co(2);
    release_now(co, 0, barrier_arr(0xB0, 0, 2));
    release_now(co, 1, barrier_arr(0xB0, 0, 2));
    REQUIRE(co.try_consume_sync(0, barrier_leave(0xB0, 0)));
    REQUIRE(co.try_consume_sync(1, barrier_leave(0xB0, 0)));

    // Iteration 1: must wait again until both arrive.
    release_now(co, 0, barrier_arr(0xB0, 1, 2));
    REQUIRE_FALSE(co.try_consume_sync(0, barrier_leave(0xB0, 1)));
    release_now(co, 1, barrier_arr(0xB0, 1, 2));
    REQUIRE(co.try_consume_sync(0, barrier_leave(0xB0, 1)));
}

TEST_CASE("Lifecycle records are recorded but never block", "[sync]") {
    SyncCoordinator co(2);
    co.on_lifecycle(0, LifecycleRecord{LifecycleKind::ThreadStart, 0, 0});
    co.on_lifecycle(0, LifecycleRecord{LifecycleKind::ThreadSpawn, 1, 0});
    co.on_lifecycle(1, LifecycleRecord{LifecycleKind::ThreadStart, 0, 0});
    REQUIRE(co.stats().lifecycle_events == 3);
    REQUIRE(co.is_blocked(0) == false);
    REQUIRE(co.is_blocked(1) == false);
}

TEST_CASE("Out-of-range tid throws", "[sync]") {
    SyncCoordinator co(2);
    REQUIRE_THROWS(co.try_consume_sync(2, lock_acq(0x1, 0)));
    REQUIRE_THROWS(co.on_lifecycle(99, LifecycleRecord{LifecycleKind::ThreadStart, 0, 0}));
}

TEST_CASE("blocked_count reflects current stalled tids", "[sync]") {
    SyncCoordinator co(3);
    REQUIRE(co.blocked_count() == 0);
    REQUIRE_FALSE(co.try_consume_sync(0, lock_acq(0x900, 5)));   // needs seq=4 first
    REQUIRE_FALSE(co.try_consume_sync(1, lock_acq(0x900, 5)));
    REQUIRE(co.blocked_count() == 2);
    // Provide the prior release (gate + retire).
    release_now(co, 2, lock_rel(0x900, 4));
    // Re-poll for tid 0 — succeeds.
    REQUIRE(co.try_consume_sync(0, lock_acq(0x900, 5)));
    REQUIRE(co.blocked_count() == 1);
}
