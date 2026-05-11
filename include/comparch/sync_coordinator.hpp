#pragma once

// SyncCoordinator: sim-global object that enforces happens-before
// relations between threads in a CasimV2 multi-thread trace replay.
//
// Model
// -----
// Each sync record in a thread's trace stream is a "fetch barrier"
// for that thread. Before the trace::Reader can advance past a sync
// record, it asks the coordinator may_proceed?. The coordinator
// matches the event against pending events from other threads
// (LockRelease ↔ LockAcquire, BarrierArrive count vs. expected,
// AtomicRMW serialization, ...). If the event's preconditions are
// satisfied the reader advances; otherwise the reader is blocked
// (Reader::blocked()==true) and the OoO core's fetch stage stalls
// for that thread that cycle. The coordinator is poll-driven from
// the reader side — it does not push notifications. When state
// changes (e.g. a lock release lands), all blocked readers re-poll
// on their next fetch attempt and the now-satisfiable ones unblock.
//
// Semantics summary
// -----------------
//   LockAcquire(obj, seq) :  requires last_released[obj] >= seq-1
//   LockRelease(obj, seq) :  always permitted; sets last_released[obj] = max(., seq)
//   AtomicRMW/Load/Store(obj, seq) : single-step lock — requires
//                                    last_done[obj] >= seq-1, then
//                                    sets last_done[obj] = seq
//   BarrierArrive(obj, seq, expected) : always permitted; bumps
//                                        arrived[obj][seq], records expected
//   BarrierLeave(obj, seq) : requires arrived[obj][seq] >= expected[obj][seq]
//
// CondWait/CondSignal/CondBroadcast are accepted but currently
// no-op (always permitted). Implementing them properly requires
// pair-matching with possible spurious wakeups; deferred until a
// trace actually contains them.
//
// Timing model
// ------------
// Signal-side events flow through the pipeline as pseudo-Insts and
// the SyncCoordinator state advances only when those pseudo-Insts
// retire. A LockAcquire on thread B that waits on a LockRelease on
// thread A unblocks only after A's release has actually committed
// — matching real-hardware release semantics. The achievable cycle
// gap between fetch-of-release and retire-of-release is bounded
// by ROB depth, so for very deep pipelines you'd see proportionally
// larger contention costs; for the project's 64-entry ROB the gap
// is modest but real (~10-30 cycles depending on the workload).
//
// Thread lifecycle records (ThreadStart/Exit/Spawn/Join) are
// recorded for visibility but do not currently gate progress —
// pre-spawn threads are assumed to start at trace position 0
// directly. Join semantics layer on once SPLASH-2 needs them.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "comparch/trace.hpp"

namespace comparch::sync {

// SyncSink interface itself lives in comparch/trace.hpp so the
// Reader can hold a SyncSink* without pulling in this header.
class SyncCoordinator final : public trace::SyncSink {
public:
    explicit SyncCoordinator(std::uint32_t num_threads);

    bool try_consume_sync(std::uint32_t tid,
                          const trace::SyncRecord& s) override;
    void notify_retire(std::uint32_t tid,
                       const trace::SyncRecord& s) override;
    void on_lifecycle(std::uint32_t tid,
                      const trace::LifecycleRecord& l) override;

    // Inspection (tests, debug reports).
    std::uint32_t num_threads() const { return num_threads_; }
    bool          is_blocked(std::uint32_t tid) const;
    std::size_t   blocked_count() const;
    // Last sequence number permitted for `obj` (-1 = none).
    // Covers locks, atomics, and barriers (last seq whose Leave
    // succeeded). Useful for assertions in tests.
    std::int64_t  last_completed_seq(std::uint64_t obj) const;

    // Stats — counters incremented over the run.
    struct Stats {
        std::uint64_t sync_consumed   = 0;
        std::uint64_t sync_stalled    = 0;   // try_consume returns false
        std::uint64_t lock_acquires   = 0;
        std::uint64_t lock_releases   = 0;
        std::uint64_t atomics         = 0;
        std::uint64_t barrier_passes  = 0;   // BarrierLeave succeeds
        std::uint64_t lifecycle_events = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    struct LockState {
        std::int64_t last_completed_seq = -1;   // matches LockRelease semantics
    };
    struct BarrierIter {
        std::uint64_t arrived  = 0;
        std::uint64_t expected = 0;             // 0 until first BarrierArrive
    };
    struct BarrierState {
        std::unordered_map<std::uint64_t, BarrierIter> by_seq;
    };

    std::uint32_t num_threads_;
    std::vector<bool> blocked_;
    // Locks and atomics share state — both are "highest serialized seq"
    // counters keyed by object address. We separate the maps anyway so
    // stats can attribute correctly and so a future analyzer can tell
    // which objects are locks vs atomics from the maps alone.
    std::unordered_map<std::uint64_t, LockState>    locks_;
    std::unordered_map<std::uint64_t, LockState>    atomics_;
    std::unordered_map<std::uint64_t, BarrierState> barriers_;
    Stats stats_;
};

} // namespace comparch::sync
