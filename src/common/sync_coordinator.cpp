#include "comparch/sync_coordinator.hpp"

#include <stdexcept>
#include <string>

namespace comparch::sync {

namespace {

// Validate tid against the configured thread count. Out-of-range
// access is a sim-builder bug, not a runtime condition the trace
// can produce; throw so it surfaces in tests.
void check_tid(std::uint32_t tid, std::uint32_t num_threads) {
    if (tid >= num_threads) {
        throw std::out_of_range(
            "SyncCoordinator: tid=" + std::to_string(tid) +
            " >= num_threads=" + std::to_string(num_threads));
    }
}

} // namespace

SyncCoordinator::SyncCoordinator(std::uint32_t num_threads)
    : num_threads_(num_threads),
      blocked_(num_threads, false) {}

// Gate check at fetch. For pure-signal events (LockRelease,
// BarrierArrive) this always returns true — no state mutation;
// the OoO core surfaces them as pseudo-Inst objects and the actual
// state advance happens in notify_retire(). For pure-gate events
// (LockAcquire, BarrierLeave) this is the only place coordinator
// state is *queried*. For atomics it's both.
bool SyncCoordinator::try_consume_sync(std::uint32_t tid,
                                       const trace::SyncRecord& s) {
    check_tid(tid, num_threads_);

    auto stall = [&] {
        blocked_[tid] = true;
        ++stats_.sync_stalled;
        return false;
    };
    auto pass = [&] {
        blocked_[tid] = false;
        ++stats_.sync_consumed;
        return true;
    };

    switch (s.kind) {
        case trace::SyncKind::LockAcquire: {
            auto& st = locks_[s.sync_object_addr];
            // For seq=0 (first acquire on this lock), required prior
            // is -1 which is the initial state — always succeeds.
            if (st.last_completed_seq + 1 < static_cast<std::int64_t>(s.sequence_no)) {
                return stall();
            }
            ++stats_.lock_acquires;
            return pass();
        }

        case trace::SyncKind::LockRelease:
            // Pure signal — passes fetch immediately, real work in
            // notify_retire (advances last_completed_seq).
            return pass();

        case trace::SyncKind::AtomicRMW:
        case trace::SyncKind::AtomicLoad:
        case trace::SyncKind::AtomicStore: {
            // Atomics gate against prior atomic at fetch, then
            // advance state at retire. Gate-check only here.
            auto& st = atomics_[s.sync_object_addr];
            if (st.last_completed_seq + 1 <
                static_cast<std::int64_t>(s.sequence_no)) {
                return stall();
            }
            return pass();
        }

        case trace::SyncKind::BarrierArrive:
            // Pure signal — arrival counter advances in notify_retire.
            return pass();

        case trace::SyncKind::BarrierLeave: {
            auto& bs = barriers_[s.sync_object_addr];
            auto it = bs.by_seq.find(s.sequence_no);
            if (it == bs.by_seq.end()) return stall();
            const auto& iter = it->second;
            if (iter.expected == 0 || iter.arrived < iter.expected) {
                return stall();
            }
            ++stats_.barrier_passes;
            return pass();
        }

        case trace::SyncKind::CondWait:
        case trace::SyncKind::CondSignal:
        case trace::SyncKind::CondBroadcast:
            return pass();
    }
    return stall();
}

// Retire-time signal. Advances coordinator state for signal-side
// events. Called by the OoO core when a sync pseudo-Inst commits.
// For pure-gate events this is a no-op (the OoO core never sees
// them as pseudo-Insts; the Reader drops them at fetch).
void SyncCoordinator::notify_retire(std::uint32_t tid,
                                    const trace::SyncRecord& s) {
    check_tid(tid, num_threads_);

    switch (s.kind) {
        case trace::SyncKind::LockRelease: {
            auto& st = locks_[s.sync_object_addr];
            const auto seq = static_cast<std::int64_t>(s.sequence_no);
            if (seq > st.last_completed_seq) st.last_completed_seq = seq;
            ++stats_.lock_releases;
            return;
        }

        case trace::SyncKind::AtomicRMW:
        case trace::SyncKind::AtomicLoad:
        case trace::SyncKind::AtomicStore: {
            auto& st = atomics_[s.sync_object_addr];
            const auto seq = static_cast<std::int64_t>(s.sequence_no);
            if (seq > st.last_completed_seq) st.last_completed_seq = seq;
            ++stats_.atomics;
            return;
        }

        case trace::SyncKind::BarrierArrive: {
            auto& it = barriers_[s.sync_object_addr].by_seq[s.sequence_no];
            if (it.expected == 0 && s.extra_arg > 0) {
                it.expected = s.extra_arg;
            }
            ++it.arrived;
            return;
        }

        // Pure-gate kinds — should never reach retire because the
        // Reader drops them at fetch on gate pass.
        case trace::SyncKind::LockAcquire:
        case trace::SyncKind::BarrierLeave:
        case trace::SyncKind::CondWait:
        case trace::SyncKind::CondSignal:
        case trace::SyncKind::CondBroadcast:
            return;
    }
}

void SyncCoordinator::on_lifecycle(std::uint32_t tid,
                                   const trace::LifecycleRecord& l) {
    check_tid(tid, num_threads_);
    ++stats_.lifecycle_events;
    (void)l;
}

bool SyncCoordinator::is_blocked(std::uint32_t tid) const {
    if (tid >= num_threads_) return false;
    return blocked_[tid];
}

std::size_t SyncCoordinator::blocked_count() const {
    std::size_t n = 0;
    for (bool b : blocked_) if (b) ++n;
    return n;
}

std::int64_t SyncCoordinator::last_completed_seq(std::uint64_t obj) const {
    if (auto it = locks_.find(obj);   it != locks_.end())   return it->second.last_completed_seq;
    if (auto it = atomics_.find(obj); it != atomics_.end()) return it->second.last_completed_seq;
    return -1;
}

} // namespace comparch::sync
