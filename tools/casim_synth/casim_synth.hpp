#pragma once

// casim_synth: fluent C++ API for hand-rolling CasimV2 multi-thread
// traces. The user describes each thread's instruction stream and
// sync events in source order, and the library writes one
// .casim per thread plus a program.manifest ready for `sim --program`.
//
// Sync-event sequence numbers are auto-assigned. Source order across
// all threads decides the lock-grant order: the first `t(i).lock(m)`
// the library sees gets seq=0, the next gets seq=1, etc. Barriers
// auto-iterate when the expected number of arrivals lands.
//
// Example
// -------
//
//     #include "casim_synth.hpp"
//     using namespace comparch::casim_synth;
//
//     int main() {
//         Program prog("lock2", /*threads=*/2);
//         auto m = prog.mutex();
//         prog.t(0).alus(100).lock(m).alus(50).unlock(m).alus(20);
//         prog.t(1).alus(10).lock(m).alus(50).unlock(m).alus(5);
//         prog.write("/tmp/lock2_out");
//         // -> /tmp/lock2_out/t0.casim, t1.casim, lock2.manifest
//     }

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "comparch/trace.hpp"

namespace comparch::casim_synth {

using MutexId   = std::uint64_t;   // synthetic VA assigned by Program
using BarrierId = std::uint64_t;

class Program;

// Proxy returned by Program::t(tid). Methods return *this so the
// caller can chain: prog.t(0).alus(10).lock(m).alus(5).unlock(m).
class ThreadHandle {
public:
    ThreadHandle& alus(std::size_t n);
    ThreadHandle& load(std::uint64_t addr);
    ThreadHandle& store(std::uint64_t addr);
    // Conditional branch with a known taken-direction. A simple way
    // to exercise the branch predictor inside a synthetic trace.
    ThreadHandle& branch(bool taken);

    // Synchronization. Locks nest; unlock pops the innermost held
    // seq for that mutex. Calling unlock without a matching prior
    // lock throws std::logic_error.
    ThreadHandle& lock(MutexId);
    ThreadHandle& unlock(MutexId);

    // Hit a barrier. The library tracks per-barrier arrival count
    // and auto-iterates when expected_participants threads have
    // arrived. Both BarrierArrive and BarrierLeave are emitted
    // back-to-back in this thread's stream — the SyncCoordinator
    // handles the fan-in via the Leave stall.
    ThreadHandle& barrier(BarrierId);

private:
    friend class Program;
    ThreadHandle(Program& p, std::uint32_t tid) : prog_(&p), tid_(tid) {}
    Program*      prog_;
    std::uint32_t tid_;
};

class Program {
public:
    Program(std::string name, std::uint32_t threads,
            std::uint64_t program_uid = 0xCA51B);
    ~Program();

    // Allocate a fresh synchronization object. The returned ID is
    // a unique 64-bit value used as the sync_object_addr in the
    // emitted SyncRecords; chosen so multiple objects can't collide.
    MutexId   mutex();
    BarrierId barrier(std::uint32_t expected_participants);

    // Per-thread proxy. tid must be in [0, threads).
    ThreadHandle t(std::uint32_t tid);

    // Write the trace files + manifest into out_dir. Creates the
    // directory if it doesn't exist. After this returns, the program
    // state is consumed and further mutations are an error.
    //
    // The manifest is named "<program_name>.manifest". Trace files
    // are named t0.casim ... t(threads-1).casim.
    void write(const std::filesystem::path& out_dir);

    // Inspection — useful for tests and for asserting expected
    // record counts before write().
    std::size_t       record_count(std::uint32_t tid) const;
    std::uint32_t     thread_count() const;
    const std::string& name() const { return name_; }

private:
    friend class ThreadHandle;

    using AnyOutRecord = std::variant<trace::Record,
                                      trace::SyncRecord,
                                      trace::LifecycleRecord>;

    struct ThreadState {
        std::vector<AnyOutRecord> records;
        // LIFO of (mutex, seq) held by this thread, so unlock can
        // pair against the matching acquire. Most programs only
        // hold one lock at a time, but nested locking is allowed.
        std::vector<std::pair<MutexId, std::uint64_t>> held_locks;
        // Walking PC counter — gives each emitted record a unique-ish
        // IP without the caller having to manage it.
        std::uint64_t next_pc = 0x10000;
    };

    struct BarrierState {
        std::uint32_t expected            = 0;
        std::uint64_t iter                = 0;
        std::uint32_t arrived_this_iter   = 0;
    };

    void emit_alu_block(std::uint32_t tid, std::size_t n);
    void emit_load(std::uint32_t tid, std::uint64_t addr);
    void emit_store(std::uint32_t tid, std::uint64_t addr);
    void emit_branch(std::uint32_t tid, bool taken);
    void emit_lock(std::uint32_t tid, MutexId m);
    void emit_unlock(std::uint32_t tid, MutexId m);
    void emit_barrier(std::uint32_t tid, BarrierId b);

    void check_tid(std::uint32_t tid) const;

    std::string                                       name_;
    std::uint64_t                                     program_uid_;
    std::vector<ThreadState>                          threads_;
    std::unordered_map<MutexId, std::uint64_t>        next_lock_seq_;
    std::unordered_map<BarrierId, BarrierState>       barriers_;
    std::uint64_t                                     next_mutex_id_   = 0x800000ULL;
    std::uint64_t                                     next_barrier_id_ = 0x900000ULL;
    bool                                              written_ = false;
};

} // namespace comparch::casim_synth
